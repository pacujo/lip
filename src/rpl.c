#include <fsdyn/charstr.h>
#include "rpl.h"
#include "util.h"
#include "intl.h"

static void append_rest(GtkTextBuffer *chat_buffer, list_elem_t *e,
                        const gchar *tag_name)
{
    if (e)
        for (;;) {
            append_text(chat_buffer, list_elem_get_value(e), tag_name);
            e = list_next(e);
            if (!e)
                break;
            append_text(chat_buffer, " ", tag_name);
        }
    append_text(chat_buffer, "\n", tag_name);
}

static void console_info(app_t *app, list_elem_t *e)
{
    GtkTextBuffer *console;
    bool at_bottom = begin_console_line(app, &console);
    append_rest(console, e, NULL);
    console_scroll_maybe(app, at_bottom);
}

FSTRACE_DECL(IRC_RPL_WELCOME, "");
FSTRACE_DECL(IRC_RPL_WELCOME_BAD_SYNTAX, "");

static bool rpl_welcome_001(app_t *app, const char *prefix, list_t *params)
{
    if (list_empty(params)) {
        FSTRACE(IRC_RPL_WELCOME_BAD_SYNTAX);
        return false;
    }
    FSTRACE(IRC_RPL_WELCOME);
    list_elem_t *e = list_get_first(params);
    reset_nick(app, list_elem_get_value(e));
    console_info(app, list_next(e));
    return true;
}

FSTRACE_DECL(IRC_RPL_AWAY, "");
FSTRACE_DECL(IRC_RPL_AWAY_BAD_SYNTAX, "");
FSTRACE_DECL(IRC_RPL_AWAY_BAD_NICK, "NICK=%s");
FSTRACE_DECL(IRC_RPL_AWAY_UNEXPECTED_NICK, "NICK=%s");

static bool rpl_away_301(app_t *app, const char *prefix, list_t *params)
{
    if (list_size(params) != 3) {
        FSTRACE(IRC_RPL_AWAY_BAD_SYNTAX);
        return false;
    }
    list_elem_t *e = list_next(list_get_first(params));
    const char *nick = list_elem_get_value(e);
    if (!valid_nick(nick)) {
        FSTRACE(IRC_RPL_AWAY_BAD_NICK, nick);
        return false;
    }
    channel_t *channel = get_channel(app, nick);
    if (!channel) {
        FSTRACE(IRC_RPL_AWAY_UNEXPECTED_NICK, nick);
        return false;
    }
    FSTRACE(IRC_RPL_AWAY);
    const char *away_msg = list_elem_get_value(list_next(e));
    append_message(channel, NULL, "log", _("%s away: %s"), nick, away_msg);
    return true;
}

FSTRACE_DECL(IRC_RPL_MOTD, "");
FSTRACE_DECL(IRC_RPL_MOTD_BAD_SYNTAX, "");

static bool rpl_motd_372(app_t *app, const char *prefix, list_t *params)
{
    if (list_empty(params)) {
        FSTRACE(IRC_RPL_MOTD_BAD_SYNTAX);
        return false;
    }
    FSTRACE(IRC_RPL_MOTD);
    console_info(app, list_next(list_get_first(params)));
    return true;
}

FSTRACE_DECL(IRC_RPL_NAMREPLY, "");
FSTRACE_DECL(IRC_RPL_NAMREPLY_BAD_SYNTAX, "");
FSTRACE_DECL(IRC_RPL_NAMREPLY_UNEXPECTED_CHANNEL, "NAME=%s");

static bool rpl_namreply_353(app_t *app, const char *prefix, list_t *params)
{
    if (list_size(params) != 4) {
        FSTRACE(IRC_RPL_NAMREPLY_BAD_SYNTAX);
        return false;
    }
    list_elem_t *e = list_next(list_get_first(params));
    const char *access_tag = list_elem_get_value(e);
    if (strlen(access_tag) != 1) {
        FSTRACE(IRC_RPL_NAMREPLY_BAD_SYNTAX);
        return false;
    }
    const char *access;
    switch (access_tag[0]) {
        case '=':
            access = _("public");
            break;
        case '*':
            access = _("private");
            break;
        case '@':
            access = _("secret");
            break;
        default:
            FSTRACE(IRC_RPL_NAMREPLY_BAD_SYNTAX);
            return false;
    }
    e = list_next(e);
    const char *name = list_elem_get_value(e);
    channel_t *channel = get_channel(app, name);
    if (!channel) {
        FSTRACE(IRC_RPL_NAMREPLY_UNEXPECTED_CHANNEL, name);
        return false;
    }
    FSTRACE(IRC_RPL_NAMREPLY);
    const char *nicks = list_elem_get_value(list_next(e));
    append_message(channel, NULL, "log",
                   _("access %s, present: %s"), access, nicks);
    return true;
}


static void default_numeric(app_t *app, const char *prefix, const char *command,
                            list_t *params)
{
    logged_command(app, prefix, command, params);
}

FSTRACE_DECL(IRC_RPL_IGNORED, "CMD=%s");

bool numeric(app_t *app, const char *prefix, const char *command,
             list_t *params)
{
    bool done = false;
    switch (atoi(command)) {
        case 1:
            done = rpl_welcome_001(app, prefix, params);
            break;
        case 301:
            done = rpl_away_301(app, prefix, params);
            break;
        case 353:
            done = rpl_namreply_353(app, prefix, params);
            break;
        case 372:
            done = rpl_motd_372(app, prefix, params);
            break;
        case 366:               /* RPL_ENDOFNAMES */
        case 376:               /* RPL_ENDOFMOTD */
            FSTRACE(IRC_RPL_IGNORED, command);
            done = true;
            break;
        default:
            ;
    }
    if (!done)
        default_numeric(app, prefix, command, params);
    return true;
}
