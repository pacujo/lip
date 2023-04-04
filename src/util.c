#include <fsdyn/charstr.h>
#include "util.h"

GtkTextBuffer *get_console(app_t *app)
{
    return gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->gui.console));
}

static bool is_console_at_bottom(app_t *app)
{
    GtkScrolledWindow *sw = GTK_SCROLLED_WINDOW(app->gui.scrolled_window);
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(sw);
    gdouble value = gtk_adjustment_get_value(adj);
    gdouble page = gtk_adjustment_get_page_size(adj);
    gdouble upper = gtk_adjustment_get_upper(adj);
    return value + page >= upper;
}

static void append_timestamp(struct tm *timestamp, GtkTextBuffer *buffer)
{
    time_t t = time(NULL);
    struct tm now;
    localtime_r(&t, &now);
    if (now.tm_year != timestamp->tm_year ||
        now.tm_yday != timestamp->tm_yday) {
        char date[100];
        strftime(date, sizeof date, "(%F)", &now);
        append_text(buffer, date, "log");
        append_text(buffer, "\n", "log");
    }
    *timestamp = now;
    char tod[100];
    strftime(tod, sizeof tod, "[%R] ", &now);
    append_text(buffer, tod, "log");
}

bool begin_console_line(app_t *app, GtkTextBuffer **console)
{
    bool at_bottom = is_console_at_bottom(app);
    *console = get_console(app);
    append_timestamp(&app->gui.timestamp, *console);
    return at_bottom;
}

static void delayed_console_scroll(app_t *app)
{
    GtkTextBuffer *buffer =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->gui.console));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(app->gui.console), &end,
                                 0.0, TRUE, 1.0, 1.0);
}

void console_scroll_maybe(app_t *app, bool scroll)
{
    /* Scolling to the bottom must be done only after GTK has had a
     * chance to readjust the GUI. g_idle_add_full() has been
     * recommended on the net, but that seems to be too soon
     * sometimes, as well... */
    if (scroll)
        async_timer_start(app->async, async_now(app->async) + 50 * ASYNC_MS,
                          (action_1) { app, (act_1) delayed_console_scroll });
}

void append_text(GtkTextBuffer *chat_buffer, const gchar *text,
                 const gchar *tag_name)
{
    if (!tag_name) {
        gtk_text_buffer_insert_at_cursor(chat_buffer, text, -1);
        return;
    }
    GtkTextIter start, end;
    gtk_text_buffer_get_end_iter(chat_buffer, &end);
    GtkTextMark *mark =
        gtk_text_buffer_create_mark(chat_buffer, "loc", &end, TRUE);
    gtk_text_buffer_insert_at_cursor(chat_buffer, text, -1);
    gtk_text_buffer_get_iter_at_mark(chat_buffer, &start, mark);
    gtk_text_buffer_get_end_iter(chat_buffer, &end);
    gtk_text_buffer_apply_tag_by_name(chat_buffer, tag_name, &start, &end);
    gtk_text_buffer_delete_mark(chat_buffer, mark);
}

static void update_cursor(GtkTextBuffer *chat_buffer)
{
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(chat_buffer, &end);
    gtk_text_buffer_place_cursor(chat_buffer, &end);
}

void append_message(channel_t *channel, const gchar *from,
                    const gchar *tag_name, const gchar *format, ...)
{
    GtkTextBuffer *chat_buffer =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(channel->chat_view));
    update_cursor(chat_buffer);
    append_timestamp(&channel->timestamp, chat_buffer);
    if (from) {
        append_text(chat_buffer, from, NULL);
        append_text(chat_buffer, ">", NULL);
    }
    va_list ap;
    va_start(ap, format);
    char *text = charstr_vprintf(format, ap);
    va_end(ap);
    append_text(chat_buffer, text, tag_name);
    fsfree(text);
    append_text(chat_buffer, "\n", NULL);
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(chat_buffer, &end);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(channel->chat_view), &end,
                                 0.05, TRUE, 0.0, 1.0);
}

bool valid_server(const char *address)
{
    return *address != '\0';    /* TBD */
}

bool valid_tcp_port(const char *port, int *number)
{
    uint64_t value;
    if (charstr_to_unsigned(port, -1, 10, &value) < 0)
        return false;
    if (value < 1 || value > 0xffff)
        return false;
    *number = value;
    return true;
}

static bool is_special(char c)
{
    switch (c) {
        case '[':
        case ']':
        case '\\':
        case '`':
        case '_':
        case '^':
        case '{':
        case '|':
        case '}':
            return true;
        default:
            return false;
    }
}

bool valid_nick(const char *nick)
{
    if (strlen(nick) > 9)
        return false;
    const char *p = nick;
    if (!(charstr_char_class(*p) & CHARSTR_ALPHA) && !is_special(*p))
        return false;
    for (;;)
        switch (*++p) {
            case '\0':
                return true;
            case '-':
                break;
            default:
                if (!(charstr_char_class(*p) & CHARSTR_ALNUM) &&
                    !is_special(*p))
                    return false;
        }
}

bool valid_name(const char *name)
{
    return true;                /* TBD */
}

void logged_command(app_t *app, const char *prefix, const char *command,
                    list_t *params)
{
    const char *mood = "log";
    GtkTextBuffer *console;
    bool at_bottom = begin_console_line(app, &console);
    append_text(console, prefix, mood);
    append_text(console, " ", mood);
    append_text(console, command, mood);
    if (!list_empty(params)) {
        append_text(console, " ", mood);
        list_elem_t *e = list_get_first(params);
        for (;;) {
            append_text(console, list_elem_get_value(e), mood);
            e = list_next(e);
            if (!e)
                break;
            append_text(console, " ▸", mood);
        }
    }
    append_text(console, "\n", mood);
    console_scroll_maybe(app, at_bottom);
}

