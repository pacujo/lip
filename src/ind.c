#include <assert.h>
#include <fsdyn/charstr.h>
#include <encjson.h>
#include "ind.h"
#include "rpl.h"
#include "util.h"
#include "intl.h"

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
                      const char *channel_name, void *user_data)
{
    channel_t *channel = open_channel(app, channel_name, 0, false);
    if (!channel) {
        if (!parts->server)
            info(app, _("%s joined %s"), parts->nick, channel->name);
        else if (parts->user)
            info(app, _("%s (%s@%s) joined %s"), parts->nick, parts->user,
                 parts->server, channel->name);
        else info(app, _("%s (%s@%s) joined %s"), parts->nick, parts->nick,
                  parts->server, channel->name);
        return;
    }
    const char *mood = "log";
    if (!parts->server)
        append_message(channel, NULL, mood, _("%s joined"), parts->nick);
    else if (parts->user)
        append_message(channel, NULL, mood, _("%s (%s@%s) joined"),
                       parts->nick, parts->user, parts->server);
    else append_message(channel, NULL, mood, _("%s (%s@%s) joined"),
                        parts->nick, parts->nick, parts->server);
}

static void distribute(app_t *app, const prefix_parts_t *parts,
                       list_elem_t *param,
                       void (*f)(app_t *app, const prefix_parts_t *parts,
                                 const char *name, void *user_data),
                       void *user_data)
{
    const char *recipients = list_elem_get_value(param);
    const char *p = recipients;
    for (;;) {
        const char *q = strchr(p, ',');
        if (!q)
            break;
        char *name = charstr_dupsubstr(p, q);
        f(app, parts, name, user_data);
        fsfree(name);
        p = q + 1;
    }
    f(app, parts, p, user_data);
}

FSTRACE_DECL(IRC_GOT_BAD_JOIN, "");
FSTRACE_DECL(IRC_GOT_OWN_JOIN, "");

static bool join(app_t *app, const char *prefix, list_t *params)
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
    if (!strcmp(parts.nick, app->config.nick)) {
        FSTRACE(IRC_GOT_OWN_JOIN);
        clear_prefix(&parts);
        return true;
    }
    distribute(app, &parts, list_get_first(params), note_join, NULL);
    clear_prefix(&parts);
    return true;
}

static bool mode(app_t *app, const char *prefix, list_t *params)
{
    logged_command(app, prefix, "MODE", params);
    return true;
}

static bool notice(app_t *app, const char *prefix, list_t *params)
{
    logged_command(app, prefix, "NOTICE", params);
    return true;
}

FSTRACE_DECL(IRC_PING_ILLEGAL, "");
FSTRACE_DECL(IRC_PONG, "SERVER=%s SERVER2=%s");

static bool ping(app_t *app, const char *prefix, list_t *params)
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
        warn(app, _("Ignore empty receiver"));
        return;
    }
    const char *sender = parts->nick;
    assert(sender);
    channel_t *channel;
    if (valid_nick(receiver)) {
        if (strcmp(receiver, app->config.nick))
            return;             /* not for me */
        channel = open_channel(app, sender, LIMIT, false);
    } else channel = open_channel(app, receiver, LIMIT, false);
    if (!channel) {
        warn(app, _("Too many channels"));
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

static void note_part(app_t *app, const prefix_parts_t *parts,
                      const char *channel_name, void *user_data)
{
    channel_t *channel = get_channel(app, channel_name);
    if (!channel)
        return;
    const char *mood = "log";
    if (!parts->server)
        append_message(channel, NULL, mood, _("%s parted"), parts->nick);
    else if (parts->user)
        append_message(channel, NULL, mood, _("%s (%s@%s) parted"),
                       parts->nick, parts->user, parts->server);
    else append_message(channel, NULL, mood, _("%s (%s@%s) parted"),
                        parts->nick, parts->nick, parts->server);
}

FSTRACE_DECL(IRC_GOT_PART, "");
FSTRACE_DECL(IRC_GOT_BAD_PART, "");

static bool part(app_t *app, const char *prefix, list_t *params)
{
    prefix_parts_t parts;
    if (list_empty(params) || !parse_prefix(prefix, &parts)) {
        FSTRACE(IRC_GOT_BAD_PART);
        return false;
    }
    FSTRACE(IRC_GOT_PART);
    distribute(app, &parts, list_get_first(params), note_part, NULL);
    clear_prefix(&parts);
    return true;
}

FSTRACE_DECL(IRC_GOT_PRIVMSG, "");
FSTRACE_DECL(IRC_GOT_BAD_PRIVMSG, "");
FSTRACE_DECL(IRC_GOT_PRIVMSG_FROM_SERVER, "SERVER=%s");

static bool privmsg(app_t *app, const char *prefix, list_t *params)
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

static json_thing_t *json_repr(const char *prefix, const char *command,
                               list_t *params)
{
    json_thing_t *msg = json_make_object();
    if (prefix)
        json_add_to_object(msg, "prefix", json_make_string(prefix));
    json_add_to_object(msg, "command", json_make_string(command));
    json_thing_t *param_array = json_make_array();
    json_add_to_object(msg, "params", param_array);
    for (list_elem_t *e = list_get_first(params); e; e = list_next(e)) {
        const char *param = list_elem_get_value(e);
        json_add_to_array(param_array, json_make_string(param));
    }
    return msg;
}

static void dump_message(app_t *app, const char *prefix, const char *command,
                         list_t *params)
{
    json_thing_t *msg = json_repr(prefix, command, params);
    size_t size = json_utf8_prettyprint(msg, NULL, 0, 0, 2);
    char encoding[size + 1];
    json_utf8_prettyprint(msg, encoding, size + 1, 0, 2);
    json_destroy_thing(msg);
    const char *mood = "log";
    GtkTextBuffer *console;
    bool at_bottom = begin_console_line(app, &console);
    append_text(console, encoding, mood);
    append_text(console, "\n", mood);
    console_scroll_maybe(app, at_bottom);
}

FSTRACE_DECL(IRC_DO_COMMAND, "MSG=%I");

bool do_it(app_t *app, const char *prefix, const char *command, list_t *params)
{
    if (FSTRACE_ENABLED(IRC_DO_COMMAND)) {
        json_thing_t *msg = json_repr(prefix, command, params);
        FSTRACE(IRC_DO_COMMAND, json_trace, msg);
        json_destroy_thing(msg);
    }
/*
 PASS <password>
 OPER <user> <password>
 QUIT [ <message> ]
 JOIN <comma-s-channels> [ <comma-s-keys> ]
 TOPIC <channel> [<topic>]
 NAMES <comma-s-channels>
 LIST [<comma-s-channels> [<server>]]
 INVITE <nick> <channel>
 KICK <channel> <user> [<comment>]
 VERSION [<server>]
 STATS [<query> [<server>]]
 LINKS [[<server>] <mask>]
 TIME [<server>]
 ADMIN [<server>]
 INFO [<server>]
 WHO [<name> [<o>]]
 WHOIS [<server>] <comma-s-masks>
 WHOWAS <nick> [<count> [<server>]]
 AWAY [<message>]
 REHASH
 USERS [<server>]
*/
    bool done = false;
    if (charstr_char_class(*command) & CHARSTR_DIGIT)
        done = numeric(app, prefix, command, params);
    else if (!strcmp(command, "JOIN"))
        done = join(app, prefix, params);
    else if (!strcmp(command, "MODE"))
        done = mode(app, prefix, params);
    else if (!strcmp(command, "NOTICE"))
        done = notice(app, prefix, params);
    else if (!strcmp(command, "PART"))
        done = part(app, prefix, params);
    else if (!strcmp(command, "PRIVMSG"))
        done = privmsg(app, prefix, params);
    else if (!strcmp(command, "PING"))
        done = ping(app, prefix, params);
    if (!done)
        dump_message(app, prefix, command, params);
    return true;
}

