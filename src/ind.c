#include <assert.h>
#include <fsdyn/charstr.h>
#include "ind.h"
#include "util.h"

typedef struct {
    char *server, *nick, *user, *host;
} prefix_parts_t;

static bool parse_prefix(const char *prefix, prefix_parts_t *parts)
{
    parts->server = parts->nick = parts->user = parts->host = NULL;
    for (const char *p = prefix;; p++)
        switch (*p) {
            case '\0':
                if (valid_nick(prefix))
                    parts->nick = charstr_dupstr(prefix);
                else parts->server = charstr_dupstr(prefix);
                return true;
            case '!':
                parts->nick = charstr_dupsubstr(prefix, p++);
                const char *q = strchr(p, '@');
                if (!q || !valid_nick(parts->nick)) {
                    fsfree(parts->nick);
                    return false;
                }
                parts->user = charstr_dupsubstr(p, q++);
                parts->host = charstr_dupstr(q);
                return true;
            case '@':
                parts->nick = charstr_dupsubstr(prefix, p++);
                if (!valid_nick(parts->nick)) {
                    fsfree(parts->nick);
                    return false;
                }
                parts->host = charstr_dupstr(p);
                return true;
            default:
                ;
        }
}

static void clear_prefix(prefix_parts_t *parts)
{
    fsfree(parts->server);
    fsfree(parts->nick);
    fsfree(parts->user);
    fsfree(parts->host);
}

static void log_line(app_t *app, const char *mood, const char *format,
                     va_list ap)
{
    GtkTextBuffer *console;
    bool at_bottom = begin_console_line(app, &console);
    char *line = charstr_vprintf(format, ap);
    append_text(console, line, mood);
    fsfree(line);
    append_text(console, "\n", mood);
    console_scroll_maybe(app, at_bottom);
}

static void info(app_t *app, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    log_line(app, "log", format, ap);
    va_end(ap);
}

static void warn(app_t *app, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    log_line(app, "error", format, ap);
    va_end(ap);
}

static void note_join(app_t *app, const prefix_parts_t *parts,
                      const char *channel_name)
{
    channel_t *channel = open_channel(app, channel_name, 0);
    if (!channel) {
        if (!parts->server)
            info(app, "%s joined %s", parts->nick, channel->name);
        else if (parts->user)
            info(app, "%s (%s@%s) joined %s", parts->nick, parts->user,
                 parts->server, channel->name);
        else info(app, "%s (%s@%s) joined %s", parts->nick, parts->nick,
                  parts->server, channel->name);
        return;
    }
    const char *mood = "log";
    if (!parts->server)
        append_message(channel, mood, "%s joined", parts->nick);
    else if (parts->user)
        append_message(channel, mood, "%s (%s@%s) joined",
                       parts->nick, parts->user, parts->server);
    else append_message(channel, mood, "%s (%s@%s) joined",
                        parts->nick, parts->nick, parts->server);
}

FSTRACE_DECL(IRC_GOT_BAD_JOIN, "");
FSTRACE_DECL(IRC_GOT_OWN_JOIN, "");

bool join(app_t *app, const char *prefix, list_t *params)
{
    prefix_parts_t parts;
    if (list_size(params) != 1 || !parse_prefix(prefix, &parts)) {
        FSTRACE(IRC_GOT_BAD_JOIN);
        return false;
    }
    if (!parts.nick) {
        FSTRACE(IRC_GOT_BAD_JOIN);
        clear_prefix(&parts);
        return false;
    }
    if (!strcmp(parts.nick, app->nick)) {
        FSTRACE(IRC_GOT_OWN_JOIN);
        clear_prefix(&parts);
        return true;
    }
    list_elem_t *e = list_get_first(params);
    const char *channels = list_elem_get_value(e);
    const char *p = channels;
    for (;;) {
        const char *q = strchr(p, ',');
        if (!q)
            break;
        char *channel_name = charstr_dupsubstr(p, q);
        note_join(app, &parts, channel_name);
        fsfree(channel_name);
        p = q + 1;
    }
    note_join(app, &parts, p);
    clear_prefix(&parts);
    return true;
}

bool mode(app_t *app, const char *prefix, list_t *params)
{
    logged_command(app, prefix, "MODE", params);
    return true;
}

bool notice(app_t *app, const char *prefix, list_t *params)
{
    logged_command(app, prefix, "NOTICE", params);
    return true;
}

FSTRACE_DECL(IRC_PING_ILLEGAL, "");
FSTRACE_DECL(IRC_PONG, "SERVER=%s SERVER2=%s");

bool ping(app_t *app, const char *prefix, list_t *params)
{
    switch (list_size(params)) {
        case 1:
        case 2:
            break;
        default:            
            FSTRACE(IRC_PING_ILLEGAL);
            return false;
    }

    list_elem_t *e1 = list_get_first(params);
    const char *s1 = list_elem_get_value(e1);
    list_elem_t *e2 = list_next(e1);
    if (!e2) {
        emit(app, "PONG :");
        emit(app, s1);
        emit(app, "\r\n");
        FSTRACE(IRC_PONG, s1, NULL);
        return true;
    }
    const char *s2 = list_elem_get_value(e2);
    emit(app, "PONG ");
    emit(app, s1);
    emit(app, " :");
    emit(app, s2);
    emit(app, "\r\n");
    FSTRACE(IRC_PONG, s1, s2);
    return true;
}

static void post(app_t *app, const prefix_parts_t *parts, const char *receiver,
                 const char *text)
{
    enum { LIMIT = 50 };
    if (!*receiver) {
        warn(app, "Ignore empty receiver");
        return;
    }
    const char *sender = parts->nick;
    assert(sender);
    channel_t *channel;
    if (valid_nick(receiver)) {
        if (strcmp(receiver, app->nick))
            return;             /* not for me */
        channel = open_channel(app, sender, LIMIT);
    } else channel = open_channel(app, receiver, LIMIT);
    if (!channel) {
        warn(app, "Too many channels");
        return;
    }
    append_message(channel, sender, "theirs", "%s", text);
}

static bool do_ctcp_version(app_t *app, const char *prefix)
{
    emit(app, "NOTICE ");
    emit(app, prefix);
    emit(app, " :\1VERSION :" APP_NAME " 0.0.1\1\n");
    return true;
}

static bool do_ctcp(app_t *app, const char *prefix, const char *text)
{
    if (!strcmp(text, "\1VERSION\1"))
        return do_ctcp_version(app, prefix);
    return false;
}

FSTRACE_DECL(IRC_GOT_PRIVMSG, "");
FSTRACE_DECL(IRC_GOT_BAD_PRIVMSG, "");
FSTRACE_DECL(IRC_GOT_PRIVMSG_FROM_SERVER, "SERVER=%s");

bool privmsg(app_t *app, const char *prefix, list_t *params)
{
    prefix_parts_t parts;
    if (list_size(params) != 2 || !parse_prefix(prefix, &parts)) {
        FSTRACE(IRC_GOT_BAD_PRIVMSG);
        return false;
    }
    if (parts.server) {
        FSTRACE(IRC_GOT_PRIVMSG_FROM_SERVER, parts.server);
        clear_prefix(&parts);
        return false;
    }
    FSTRACE(IRC_GOT_PRIVMSG);
    list_elem_t *e = list_get_first(params);
    const char *receivers = list_elem_get_value(e);
    const char *text = list_elem_get_value(list_next(e));
    if (text[0] == '\1') {
        clear_prefix(&parts);
        return do_ctcp(app, prefix, text);
    }
    const char *p = receivers;
    for (;;) {
        const char *q = strchr(p, ',');
        if (!q)
            break;
        char *receiver = charstr_dupsubstr(p, q);
        post(app, &parts, receiver, text);
        fsfree(receiver);
        p = q + 1;
    }
    post(app, &parts, p, text);
    clear_prefix(&parts);
    return true;
}
