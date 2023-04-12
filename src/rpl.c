#include <fsdyn/charstr.h>
#include "rpl.h"
#include "util.h"

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
    const char *mood = NULL;
    GtkTextBuffer *console;
    bool at_bottom = begin_console_line(app, &console);
    append_rest(console, e, mood);
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
    fsfree(app->config.nick);
    app->config.nick = charstr_dupstr(list_elem_get_value(e));
    char *title = charstr_printf("%s@%s", APP_NAME, app->config.nick);
    gtk_window_set_title(GTK_WINDOW(app->gui.app_window), title);
    fsfree(title);
    console_info(app, list_next(e));
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
        case 372:
            done = rpl_motd_372(app, prefix, params);
            break;
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
