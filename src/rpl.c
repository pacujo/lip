#include <fsdyn/charstr.h>
#include "rpl.h"
#include "util.h"

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
    fsfree(app->nick);
    app->nick = charstr_dupstr(list_elem_get_value(e));
    char *title = charstr_printf("%s@%s", APP_NAME, app->nick);
    gtk_window_set_title(GTK_WINDOW(app->gui.app_window), title);
    fsfree(title);
    e = list_next(e);
    const char *mood = NULL;
    GtkTextBuffer *console;
    bool at_bottom = begin_console_line(app, &console);
    if (e)
        for (;;) {
            append_text(console, list_elem_get_value(e), mood);
            e = list_next(e);
            if (!e)
                break;
            append_text(console, " ", mood);
        }
    append_text(console, "\n", mood);
    console_scroll_maybe(app, at_bottom);
    return true;
}

static void default_numeric(app_t *app, const char *prefix, const char *command,
                            list_t *params)
{
    logged_command(app, prefix, command, params);
}

bool numeric(app_t *app, const char *prefix, const char *command,
             list_t *params)
{
    bool done = false;
    switch (atoi(command)) {
        case 1:
            done = rpl_welcome_001(app, prefix, params);
            break;
        default:
            ;
    }
    if (!done)
        default_numeric(app, prefix, command, params);
    return true;
}
