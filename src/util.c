#include <fcntl.h>
#include <sys/stat.h>
#include <assert.h>
#include <encjson.h>
#include <fsdyn/charstr.h>
#include <fsdyn/integer.h>
#include "util.h"
#include "intl.h"
#include "url.h"

static const char *const IRC_DEFAULT_SERVER = "irc.oftc.net";
static const int IRC_DEFAULT_PORT = 6697;
static const bool IRC_DEFAULT_USE_TLS = true;
static const char *const IRC_DEFAULT_CACHE_DIR = ".cache/lip/main";

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

const char *TIMESTAMP_PATTERN = "[%R] ";

static void append_timestamp(struct tm *timestamp, time_t t,
                             GtkTextBuffer *buffer)
{
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
    strftime(tod, sizeof tod, TIMESTAMP_PATTERN, &now);
    append_text(buffer, tod, "log");
}

static gboolean ignore_key_press(GtkWidget *, GdkEventKey *, gpointer)
{
    return TRUE;
}

static void text_dimensions(const char *text, int *width, int *height)
{
    GtkEntryBuffer *buf = gtk_entry_buffer_new(NULL, -1);
    GtkWidget *entry = gtk_entry_new_with_buffer(buf);
    PangoLayout *layout = gtk_widget_create_pango_layout(entry, text);
    pango_layout_get_pixel_size(layout, width, height);
    g_clear_object(&layout);
    g_clear_object(&buf);
}

int one_em()
{
    static int em = -1;
    if (em < 0)
        text_dimensions("m", &em, NULL);
    return em;
}

int one_ex()
{
    static int ex = -1;
    if (ex < 0)
        text_dimensions("x", NULL, &ex);
    return ex;
}

static int timestamp_width()
{
    static int width = -1;
    if (width < 0) {
        struct tm zero = { 0 };
        char tod[100];
        strftime(tod, sizeof tod, TIMESTAMP_PATTERN, &zero);
        text_dimensions(tod, &width, NULL);
    }
    return width;
}

bool begin_console_line(app_t *app, GtkTextBuffer **console)
{
    bool at_bottom = is_console_at_bottom(app);
    *console = get_console(app);
    append_timestamp(&app->gui.timestamp, time(NULL), *console);
    return at_bottom;
}

static void delayed_console_scroll(app_t *app)
{
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(app->gui.console),
                                       app->gui.end_of_console);
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

char *escape_xml(const char *text)
{
    list_t *snippets = make_list();
    const char *p = text;
    const char *q = p;
    for (;;)
        switch (*q) {
            case '\0':
                list_append(snippets, charstr_dupstr(p));
                char *escaped = charstr_join("", snippets);
                list_foreach(snippets, (void *) fsfree, NULL);
                destroy_list(snippets);
                return escaped;
            case '&':
                list_append(snippets, charstr_dupsubstr(p, q));
                list_append(snippets, charstr_dupstr("&amp;"));
                p = ++q;
                break;
            case '<':
                list_append(snippets, charstr_dupsubstr(p, q));
                list_append(snippets, charstr_dupstr("&lt;"));
                p = ++q;
                break;
            default:
                q++;
        }
}

static void span(char **escaped_text, const char *key, const char *value)
{
    char *spanned_text =
        charstr_printf("<span %s='%s'>%s</span>", key, value, *escaped_text);
    fsfree(*escaped_text);
    *escaped_text = spanned_text;
}

static void tag_text(char **escaped_text, const char *tag_name)
{
    if (!tag_name)
        return;
    if (!strcmp(tag_name, "mine")) {
        span(escaped_text, "foreground", "blue");
        return;
    }
    if (!strcmp(tag_name, "theirs")) {
        span(escaped_text, "foreground", "red");
        return;
    }
    if (!strcmp(tag_name, "log")) {
        span(escaped_text, "foreground", "cyan");
        return;
    }
    if (!strcmp(tag_name, "error")) {
        span(escaped_text, "foreground", "red");
        return;
    }
    span(escaped_text, "strikethrough", "true");
}

typedef struct {
    bool bold, underline, italic;
    unsigned fg_color, bg_color;
} irc_text_style_t;

static const char *adjust_style(const char *q, irc_text_style_t *style)
{
    switch (*q++) {
        default:
            abort();
        case 'B' & 0x1f:
            style->bold = !style->bold;
            return q;
        case 'O' & 0x1f:
            style->bold = style->underline = style->italic = false;
            style->fg_color = style->bg_color = -1U;
            return q;
        case 'R' & 0x1f:
            style->italic = !style->italic;
            return q;
        case 'U' & 0x1f:
            style->underline = !style->underline;
            return q;
        case 'C' & 0x1f:
            ;
    }
    if (!(charstr_char_class(*q) & CHARSTR_DIGIT)) {
        style->fg_color = style->bg_color = -1;
        return q;
    }
    style->fg_color = *q++ - '0';
    if (charstr_char_class(*q) & CHARSTR_DIGIT)
        style->fg_color = style->fg_color * 10 + *q++ - '0';
    if (q[0] != ',' || !(charstr_char_class(q[1]) & CHARSTR_DIGIT))
        return q;
    style->bg_color = *++q - '0';
    if (charstr_char_class(*++q) & CHARSTR_DIGIT)
        style->bg_color = style->bg_color * 10 + *q++ - '0';
    return q;
}

static void append_snippet(GtkTextBuffer *chat_buffer, const char *p,
                           const char *q, const irc_text_style_t *style,
                           const char *tag_name, GtkTextIter *end)
{
    if (p == q)
        return;
    char *snippet = charstr_dupsubstr(p, q);
    if (style->bold)
        span(&snippet, "weight", "bold");
    if (style->italic)
        span(&snippet, "style", "italic");
    if (style->underline)
        span(&snippet, "underline", "single");
    static const char *const colors[16] = {
        "white", "black", "blue", "green", "red", "brown", "purple", "orange",
        "yellow", "lightgreen", "cyan", "lightcyan", "lightblue", "pink",
        "grey", "lightgrey"
    };
    if (style->fg_color < 16)
        span(&snippet, "foreground", colors[style->fg_color]);
    if (style->bg_color < 16)
        span(&snippet, "background", colors[style->bg_color]);
    tag_text(&snippet, tag_name);
    gtk_text_buffer_insert_markup(chat_buffer, end, snippet, -1);
    fsfree(snippet);
}

void append_text(GtkTextBuffer *chat_buffer, const gchar *text,
                 const char *tag_name)
{
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(chat_buffer, &end);
    char *escaped = escape_xml(text);
    irc_text_style_t style = {
        .fg_color = -1U,
        .bg_color = -1U,
    };
    const char *p = escaped;
    const char *q = p;
    for (;;)
        switch (*q) {
            case '\0':
                append_snippet(chat_buffer, p, q, &style, tag_name, &end);
                fsfree(escaped);
                return;
            case 'B' & 0x1f:
            case 'C' & 0x1f:
            case 'O' & 0x1f:
            case 'R' & 0x1f:
            case 'U' & 0x1f:
                append_snippet(chat_buffer, p, q, &style, tag_name, &end);
                p = q = adjust_style(q, &style);
                break;
            default:
                q++;
        }
}

static bool is_date_line(const gchar *line)
{
    /* Each line begins either with a date or a time of day. Dates
     * begin with a parenthesis. Times begin with a bracket. */
    return line[0] == '(';
}

static void forget_old_message(GtkTextBuffer *chat_buffer)
{
    /* The first line is the date; the second line is not the date */
    GtkTextIter line_start, line_end;
    gtk_text_buffer_get_iter_at_line(chat_buffer, &line_start, 1);
    gtk_text_buffer_get_iter_at_line(chat_buffer, &line_end, 2);
    gtk_text_buffer_delete(chat_buffer, &line_start, &line_end);
    /* Do we now have two dates in a row? */
    gtk_text_buffer_get_iter_at_line(chat_buffer, &line_start, 1);
    gtk_text_buffer_get_iter_at_line(chat_buffer, &line_end, 2);
    gchar *line = gtk_text_buffer_get_text(chat_buffer,
                                           &line_start, &line_end, FALSE);
    if (is_date_line(line)) {
        /* Yes, we do: remove the redundant first date. */
        GtkTextIter start;
        gtk_text_buffer_get_start_iter(chat_buffer, &start);
        gtk_text_buffer_delete(chat_buffer, &start, &line_start);
    }
    g_free(&line);
}

void play_message(channel_t *channel, time_t t, const char *from,
                  const char *tag_name, const char *text)
{
    enum { MAX_LINE_COUNT = 1000 };
    GtkTextBuffer *chat_buffer =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(channel->chat_view));
    while (gtk_text_buffer_get_line_count(chat_buffer) >= MAX_LINE_COUNT)
        forget_old_message(chat_buffer);
    append_timestamp(&channel->timestamp, t, chat_buffer);
    if (from) {
        append_text(chat_buffer, from, NULL);
        append_text(chat_buffer, ">", NULL);
    }
    append_text(chat_buffer, text, tag_name);
    append_text(chat_buffer, "\n", NULL);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(channel->chat_view),
                                       channel->end_of_chat_view);
}

static void log_message(channel_t *channel, time_t t, const char *from,
                        const char *tag_name, const char *text)
{
    app_t *app = channel->app;
    struct tm umt_stamp;
    gmtime_r(&t, &umt_stamp);
    switch (rotatable_rotate_maybe(app->cache, &umt_stamp, 0, false)) {
        default:
            return;             /* ? */
        case ROTATION_OK:
        case ROTATION_ROTATED:
            ;
    }
    FILE *cachef = rotatable_file(app->cache);
    assert(cachef);
    json_thing_t *message = json_make_object();
    json_add_to_object(message, "channel", json_make_string(channel->key));
    json_add_to_object(message, "time", json_make_unsigned(t));
    if (from)
        json_add_to_object(message, "from", json_make_string(from));
    if (tag_name)
        json_add_to_object(message, "tag", json_make_string(tag_name));
    json_add_to_object(message, "text", json_make_string(text));
    size_t size = json_utf8_encode(message, NULL, 0) + 1;
    char *encoding = fsalloc(size);
    json_utf8_encode(message, encoding, size);
    json_destroy_thing(message);
    fwrite(encoding, size, 1, cachef); /* include the terminating '\0' */
    fsfree(encoding);
    fflush(cachef);
}

void append_message(channel_t *channel, const gchar *from,
                    const gchar *tag_name, const gchar *format, ...)
{
    va_list ap;
    va_start(ap, format);
    char *text = charstr_vprintf(format, ap);
    va_end(ap);
    time_t t = time(NULL);
    play_message(channel, t, from, tag_name, text);
    log_message(channel, t, from, tag_name, text);
    fsfree(text);
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

bool valid_nick(const char *nick)
{
    switch (*nick) {
        case '\0':
        case '$':
        case ':':
        case '#':
        case '&':
            return false;
        default:
            ;
    }
    for (const char *p = nick; *p; p++)
        switch (*p) {
            case ' ':
            case ',':
            case '*':
            case '?':
            case '!':
            case '@':
            case '.':
                return false;
            default:
                if (charstr_char_class(*p) & CHARSTR_CONTROL)
                    return false;
        }
    return true;
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

static void get_app_settings(app_t *app, json_thing_t *cfg)
{
    const char *nick;
    if (json_object_get_string(cfg, "nick", &nick)) {
        fsfree(app->config.nick);
        app->config.nick = charstr_dupstr(nick);
    }
    const char *full_name;
    if (json_object_get_string(cfg, "full_name", &full_name)) {
        fsfree(app->config.name);
        app->config.name = charstr_dupstr(full_name);
    }
    const char *server;
    if (json_object_get_string(cfg, "server", &server)) {
        fsfree(app->config.server);
        app->config.server = charstr_dupstr(server);
    }
    long long port;
    if (json_object_get_integer(cfg, "port", &port))
        app->config.port = port;
    bool use_tls;
    if (json_object_get_boolean(cfg, "use_tls", &use_tls))
        app->config.use_tls = use_tls;
}

void destroy_channel_id(channel_id_t *chid)
{
    fsfree(chid->key);
    fsfree(chid->name);
    fsfree(chid);
}

void clear_autojoins(app_t *app)
{
    while (!avl_tree_empty(app->config.autojoins)) {
        avl_elem_t *ae = avl_tree_pop_first(app->config.autojoins);
        channel_id_t *chid = (channel_id_t *) avl_elem_get_value(ae);
        destroy_channel_id(chid);
        destroy_avl_element(ae);
    }
}

static void get_channel_settings(app_t *app, json_thing_t *cfg)
{
    clear_autojoins(app);
    json_thing_t *channel_cfgs;
    if (!json_object_get_array(cfg, "channels", &channel_cfgs))
        return;
    for (json_element_t *je = json_array_first(channel_cfgs); je;
         je = json_element_next(je)) {
        json_thing_t *channel_cfg = json_element_value(je);
        const char *name;
        if (json_thing_type(channel_cfg) != JSON_OBJECT ||
            !json_object_get_string(channel_cfg, "name", &name))
            continue;
        set_autojoin(app, name, true);
    }
}

void load_session(app_t *app)
{
    app->config.nick = charstr_dupstr("");
    app->config.name = charstr_dupstr("");
    app->config.server = charstr_dupstr(IRC_DEFAULT_SERVER);
    app->config.port = IRC_DEFAULT_PORT;
    app->config.use_tls = IRC_DEFAULT_USE_TLS;
    app->config.cache_directory =
        charstr_printf("%s/%s", app->home_dir, IRC_DEFAULT_CACHE_DIR);
    if (app->opts.reset || !app->opts.config_file)
        return;
    FILE *cfgf = fopen(app->opts.config_file, "r");
    if (!cfgf)
        return;
    json_thing_t *cfg = json_utf8_decode_file(cfgf, 1000000);
    fclose(cfgf);
    if (!cfg)
        return;
    if (json_thing_type(cfg) != JSON_OBJECT) {
        json_destroy_thing(cfg);
        return;
    }
    get_app_settings(app, cfg);
    get_channel_settings(app, cfg);
    json_destroy_thing(cfg);
}

static json_thing_t *build_settings(app_t *app)
{
    json_thing_t *cfg = json_make_object();
    json_add_to_object(cfg, "nick", json_make_string(app->config.nick));
    json_add_to_object(cfg, "full_name", json_make_string(app->config.name));
    json_add_to_object(cfg, "server", json_make_string(app->config.server));
    json_add_to_object(cfg, "port", json_make_integer(app->config.port));
    json_add_to_object(cfg, "use_tls", json_make_boolean(app->config.use_tls));
    json_thing_t *channel_cfgs = json_make_array();
    json_add_to_object(cfg, "channels", channel_cfgs);
    for (avl_elem_t *ae = avl_tree_get_first(app->config.autojoins); ae;
         ae = avl_tree_next(ae)) {
        channel_id_t *chid = (channel_id_t *) avl_elem_get_value(ae);
        json_thing_t *channel_cfg = json_make_object();
        json_add_to_array(channel_cfgs, channel_cfg);
        json_add_to_object(channel_cfg, "name", json_make_string(chid->name));
    }
    return cfg;
}

void make_parent_dirs(const char *pathname)
{
    char *copy = charstr_dupstr(pathname);
    for (char *p = copy; *p; p++)
        if (*p == '/') {
            *p = '\0';
            mkdir(copy, S_IRWXU);
            *p = '/';
        }
    fsfree(copy);
}

void save_session(app_t *app)
{
    if (!app->opts.config_file)
        return;
    make_parent_dirs(app->opts.config_file);
    FILE *cfgf = fopen(app->opts.config_file, "w");
    if (!cfgf) {
        fprintf(stderr, _(PROGRAM ": cannot open %s\n"), app->opts.config_file);
        return;
    }
    json_thing_t *cfg = build_settings(app);
    json_utf8_dump(cfg, cfgf);
    json_destroy_thing(cfg);
    fclose(cfgf);
}

void set_autojoin(app_t *app, const char *name, bool enabled)
{
    char *key = lcase_string(name);
    avl_elem_t *ae = avl_tree_get(app->config.autojoins, key);
    if (enabled) {
        if (ae) {
            fsfree(key);
            return;
        }
        channel_id_t *chid = fsalloc(sizeof *chid);
        chid->key = key;
        chid->name = charstr_dupstr(name);
        avl_tree_put(app->config.autojoins, key, chid);
    } else {
        fsfree(key);
        if (!ae)
            return;
        destroy_channel_id((channel_id_t *) avl_elem_get_value(ae));
        avl_tree_remove(app->config.autojoins, ae);
    }
}

static char scandinavian_lcase(char c)
{
    switch (c) {
        case '[':
            return '{';
        case ']':
            return '}';
        case '\\':
            return '|';
        case '~':
            return '^';
        default:
            return charstr_lcase_char(c);
    }
}

char *lcase_string(const char *name)
{
    char *key = charstr_dupstr(name);
    for (char *s = key; *s; s++)
        *s = scandinavian_lcase(*s);
    return key;
}

GtkWidget *build_passive_text_view()
{
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD);
    gtk_text_view_set_indent(GTK_TEXT_VIEW(view),
                             -timestamp_width() - one_em());
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
    g_signal_connect(G_OBJECT(view), "key_press_event",
                     G_CALLBACK(ignore_key_press), NULL);
    return view;
}

static GtkWidget *build_prompt(channel_t *channel)
{
    GtkWidget *view = build_passive_text_view(channel->app);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    gtk_text_buffer_set_text(buffer, "⇨", -1);
    return view;
}

bool is_enter_key(GdkEventKey *event)
{
    switch (event->keyval) {
        case GDK_KEY_Return:
        case GDK_KEY_3270_Enter:
        case GDK_KEY_ISO_Enter:
        case GDK_KEY_KP_Enter:
            return true;
        default:
            return false;
    }
}

static gchar *extract_text(GtkTextBuffer *buffer)
{
    GtkTextIter start, end;
    gtk_text_buffer_get_start_iter(buffer, &start);
    gtk_text_buffer_get_end_iter(buffer, &end);
    return gtk_text_buffer_get_text(buffer, &start, &end, TRUE);
}

void modal_error_dialog(GtkWidget *parent, const gchar *text)
{
    GtkWidget *error_dialog =
        gtk_message_dialog_new(GTK_WINDOW(parent),
                               GTK_DIALOG_DESTROY_WITH_PARENT |
                               GTK_DIALOG_MODAL,
                               GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, text);
    gtk_widget_show_all(error_dialog);
    gtk_dialog_run(GTK_DIALOG(error_dialog));
    gtk_widget_destroy(error_dialog);
}

static bool send_message(channel_t *channel, const gchar *text)
{
    char *message = charstr_printf("PRIVMSG %s :%s\r\n", channel->name, text);
    if (strlen(message) > 512) {
        fsfree(message);
        return false;
    }
    emit(channel->app, message);
    fsfree(message);
    return true;
}

static const char *BOLD_MARKUP = "🄱";
static const char *ITALIC_MARKUP = "🄸";
static const char *UNDERLINE_MARKUP = "🅄";
static const char *ORIGINAL_MARKUP = "🄾";
static const char *COLOR_MARKUP = "🄲";
static const char *HIDE_MARKUP = "🗝";

enum {
    BOLD_CONTROL = 'B' & 0x1f,
    ITALIC_CONTROL = 'R' & 0x1f,
    UNDERLINE_CONTROL = 'U' & 0x1f,
    ORIGINAL_CONTROL = 'O' & 0x1f,
    COLOR_CONTROL = 'C' & 0x1f,
};

static char *markup_to_wire(const gchar *text)
{
    size_t length = strlen(text);
    char *converted = fsalloc(length + 1);
    char *q = converted;
    const char *p = text;
    const char *next;
    for (; *p; p = next) {
        if ((next = charstr_skip_prefix(p, BOLD_MARKUP)))
            *q++ = BOLD_CONTROL;
        else if ((next = charstr_skip_prefix(p, ITALIC_MARKUP)))
            *q++ = ITALIC_CONTROL;
        else if ((next = charstr_skip_prefix(p, UNDERLINE_MARKUP)))
            *q++ = UNDERLINE_CONTROL;
        else if ((next = charstr_skip_prefix(p, ORIGINAL_MARKUP)))
            *q++ = ORIGINAL_CONTROL;
        else if ((next = charstr_skip_prefix(p, COLOR_MARKUP)))
            *q++ = COLOR_CONTROL;
        else if (!(next = charstr_skip_prefix(p, HIDE_MARKUP))) {
            next = charstr_decode_utf8_codepoint(p, NULL, NULL);
            while (p != next)
                *q++ = *p++;
        }
    }
    *q = '\0';
    return converted;
}

static char *markup_to_archive(const gchar *text)
{
    size_t length = strlen(text);
    char *converted = fsalloc(length + 1);
    char *q = converted;
    const char *p = text;
    const char *next;
    for (; *p; p = next)
        if ((next = charstr_skip_prefix(p, BOLD_MARKUP)))
            *q++ = BOLD_CONTROL;
        else if ((next = charstr_skip_prefix(p, ITALIC_MARKUP)))
            *q++ = ITALIC_CONTROL;
        else if ((next = charstr_skip_prefix(p, UNDERLINE_MARKUP)))
            *q++ = UNDERLINE_CONTROL;
        else if ((next = charstr_skip_prefix(p, ORIGINAL_MARKUP)))
            *q++ = ORIGINAL_CONTROL;
        else if ((next = charstr_skip_prefix(p, COLOR_MARKUP)))
            *q++ = COLOR_CONTROL;
        else if ((next = charstr_skip_prefix(p, HIDE_MARKUP))) {
            while (p != next)
                *q++ = *p++;
            for (; *p; p = next) {
                if ((next = charstr_skip_prefix(p, HIDE_MARKUP)))
                    break;
                next = charstr_decode_utf8_codepoint(p, NULL, NULL);
            }
        } else {
            next = charstr_decode_utf8_codepoint(p, NULL, NULL);
            while (p != next)
                *q++ = *p++;
        }
    *q = '\0';
    return converted;
}

static bool nick_break(int codepoint)
{
    switch (charstr_unicode_category(codepoint)) {
        case UNICODE_CATEGORY_Ll:
        case UNICODE_CATEGORY_Lm:
        case UNICODE_CATEGORY_Lo:
        case UNICODE_CATEGORY_Lt:
        case UNICODE_CATEGORY_Lu:
        case UNICODE_CATEGORY_Nd:
        case UNICODE_CATEGORY_Nl:
        case UNICODE_CATEGORY_No:
            return false;
        default:
            return true;
    }
}

static const char *skip_nick(channel_t *channel, const char *s)
{
    for (list_elem_t *e = list_get_first(channel->nicks_present); e;
         e = list_next(e)) {
        const char *nick = list_elem_get_value(e);
        const char *skipped = charstr_skip_prefix(s, nick);
        if (!skipped)
            continue;
        int codepoint;
        if (!charstr_decode_utf8_codepoint(skipped, NULL, &codepoint) ||
            nick_break(codepoint))
            return skipped;
    }
    return NULL;
}

static char *wedge(const char *text, list_t *points, const char *joiner)
{
    list_t *snippets = make_list();
    const char *p = text;
    for (list_elem_t *e = list_get_first(points); e; e = list_next(e)) {
        const char *q = text + as_intptr(list_elem_get_value(e));
        list_append(snippets, charstr_dupsubstr(p, q));
        p = q;
    }
    list_append(snippets, charstr_dupstr(p));
    char *result = charstr_join(joiner, snippets);
    list_foreach(snippets, (void *) fsfree, NULL);
    destroy_list(snippets);
    return result;
}

static char *highlight_nicks(channel_t *channel, const char *text)
{
    list_t *points = make_list();
    char *lcase = lcase_string(text);
    const char *s = lcase;
    while (*s) {
        const char *skipped = skip_nick(channel, s);
        if (skipped) {
            list_append(points, as_integer(s - lcase));
            list_append(points, as_integer(skipped - lcase));
            s = skipped;
            continue;
        }
        for (;; s++) {
            int codepoint;
            const char *next =
                charstr_decode_utf8_codepoint(s, NULL, &codepoint);
            if (!next) {
                s++;
                break;
            }
            if (nick_break(codepoint)) {
                s = next;
                break;
            }
        }
    }
    fsfree(lcase);
    static const char NICK_WEDGE[] = { BOLD_CONTROL, '\0' };
    char *highlighted = wedge(text, points, NICK_WEDGE);
    destroy_list(points);
    return highlighted;
}

static char *highlight_urls(const char *text)
{
    list_t *points = make_list();
    const char *p = text;
    for (;;) {
        const char *url_end;
        const char *url = find_url(p, NULL, &url_end);
        if (!url)
            break;
        list_append(points, as_integer(url - text));
        list_append(points, as_integer(url_end - text));
        p = url_end;
    }
    static const char URL_WEDGE[] = { UNDERLINE_CONTROL, '\0' };
    char *highlighted = wedge(text, points, URL_WEDGE);
    destroy_list(points);
    return highlighted;
}

char *highlight(channel_t *channel, const char *text)
{
    char *h_nicks = highlight_nicks(channel, text);
    char *h_urls = highlight_urls(h_nicks);
    fsfree(h_nicks);
    return h_urls;
}

static gboolean on_key_press(GtkWidget *view, GdkEventKey *event,
                             channel_t *channel)
{
    if (!is_enter_key(event))
        return FALSE;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    gchar *text = extract_text(buffer);
    const gchar *msg_text = text;
    switch (text[0]) {
        case '\0':
            /* Just ignore an empty message. */
            g_free(text);
            return TRUE;
        case '/':
            /* Since traditional IRC clients use slashes to prefix commands,
             * we require that an initial slash be doubled. */
            if (text[1] == '/') {
                msg_text++;
                break;
            }
            g_free(text);
            modal_error_dialog(channel->window,
                               _("If you really want to send an initial '/', "
                                 "double it"));
            return TRUE;
        default:
            ;
    }
    char *marked_up = markup_to_wire(msg_text);
    bool ok = send_message(channel, marked_up);
    fsfree(marked_up);
    if (!ok) {
        g_free(text);
        modal_error_dialog(channel->window, _("Message too long"));
        return TRUE;
    }
    char *archived = markup_to_archive(msg_text);
    g_free(text);
    char *highlighted = highlight(channel, archived);
    fsfree(archived);
    gtk_text_buffer_set_text(buffer, "", -1);
    append_message(channel, channel->app->config.nick, "mine", "%s",
                   highlighted);
    fsfree(highlighted);
    return TRUE;
}

static GtkWidget *build_send_pane(channel_t *channel)
{
    channel->input_view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(channel->input_view),
                                GTK_WRAP_WORD);
    g_signal_connect(G_OBJECT(channel->input_view), "key_press_event",
                     G_CALLBACK(on_key_press), channel);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sw), channel->input_view);
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(hbox), build_prompt(channel), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), sw, TRUE, TRUE, 0);
    return hbox;
}

static void destroy_channel_window(GtkWidget *, channel_t *channel)
{
    channel->window = NULL;
}

static int message_log_filter(const struct dirent *entity)
{
    return charstr_skip_prefix(entity->d_name, "messages") != NULL &&
        charstr_ends_with(entity->d_name, ".log");
}

static int message_log_cmp(const struct dirent **a, const struct dirent **b)
{
    return strcmp((*a)->d_name, (*b)->d_name);
}

char *read_file(const char *pathname, size_t *count)
{
    enum { MAX_SIZE = 1000000 };
    FILE *f = fopen(pathname, "r");
    if (!f)
        return NULL;
    char *content = fsalloc(MAX_SIZE);
    *count = fread(content, 1, MAX_SIZE, f);
    fclose(f);
    return content;
}

static void replay_channel(channel_t *channel)
{
    app_t *app = channel->app;
    struct dirent **namelist;
    int n = scandir(app->config.cache_directory, &namelist,
                    message_log_filter, message_log_cmp);
    assert(n >= 0);
    for (int i = 0; i < n; i++) {
        char *path = charstr_printf("%s/%s", app->config.cache_directory,
                                    namelist[i]->d_name);
        free(namelist[i]);
        size_t count;
        char *content = read_file(path, &count);
        fsfree(path);
        if (!content)
            continue;
        const char *end = content + count;
        const char *cursor = content;
        const char *p = content;
        while (p < end)
            if (!*p++) {
                json_thing_t *message = json_utf8_decode_string(cursor);
                if (message) {
                    const char *key, *text;
                    unsigned long long t;
                    if (json_object_get_string(message, "channel", &key) &&
                        !strcmp(key, channel->key) &&
                        json_object_get_unsigned(message, "time", &t) &&
                        json_object_get_string(message, "text", &text)) {
                        const char *from, *tag;
                        if (!json_object_get_string(message, "from", &from))
                            from = NULL;
                        if (!json_object_get_string(message, "tag", &tag))
                            tag = NULL;
                        play_message(channel, t, from, tag, text);
                    }
                    json_destroy_thing(message);
                }
                cursor = p;
            }
        fsfree(content);
    }
    free(namelist);
}

static void close_activated(GSimpleAction *action, GVariant *parameter,
                            gpointer user_data)
{
    GtkWidget *window = user_data;
    gtk_window_close(GTK_WINDOW(window));
}

static void autojoin_changed(GSimpleAction *action, GVariant *value,
                             channel_t *channel)
{
    channel->autojoin = g_variant_get_boolean(value);
    g_simple_action_set_state(action, value);
    set_autojoin(channel->app, channel->name, channel->autojoin);
    save_session(channel->app);
}

static void mark_up_input(channel_t *channel, const char *markup)
{
    GtkTextBuffer *buffer =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(channel->input_view));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end, markup, -1);
}

static void bold_activated(GSimpleAction *action, GVariant *parameter,
                            gpointer user_data)
{
    mark_up_input(user_data, BOLD_MARKUP);
}

static void italic_activated(GSimpleAction *action, GVariant *parameter,
                             gpointer user_data)
{
    mark_up_input(user_data, ITALIC_MARKUP);
}

static void underline_activated(GSimpleAction *action, GVariant *parameter,
                                gpointer user_data)
{
    mark_up_input(user_data, UNDERLINE_MARKUP);
}

static void original_activated(GSimpleAction *action, GVariant *parameter,
                               gpointer user_data)
{
    mark_up_input(user_data, ORIGINAL_MARKUP);
}

static void hide_activated(GSimpleAction *action, GVariant *parameter,
                             gpointer user_data)
{
    mark_up_input(user_data, HIDE_MARKUP);
}

static void color_activated(GSimpleAction *action, GVariant *parameter,
                            gpointer user_data)
{
    GValue value = { 0 };
    g_object_get_property(G_OBJECT(action), "name", &value);
    const char *suffix =
        charstr_skip_prefix(g_value_get_string(&value), "color");
    if (suffix[0]) {
        char *markup;
        if (suffix[2])
            markup = charstr_printf("%s%c%c,%c%c", COLOR_MARKUP,
                                    suffix[0], suffix[1], suffix[2], suffix[3]);
        else markup = charstr_printf("%s%c%c", COLOR_MARKUP,
                                     suffix[0], suffix[1]);
        mark_up_input(user_data, markup);
        fsfree(markup);
    } else mark_up_input(user_data, COLOR_MARKUP);
    g_value_unset(&value);
}

static void add_color_action(channel_t *channel, GActionGroup *actions,
                             const char *name)
{
    GActionEntry entry = { name, color_activated };
    g_action_map_add_action_entries(G_ACTION_MAP(actions), &entry, 1, channel);
}

static void add_color_actions(channel_t *channel, GActionGroup *actions)
{
    add_color_action(channel, actions, "color");
    for (int fg = 0; fg < 16; fg++) {
        char *name = charstr_printf("color%02d", fg);
        add_color_action(channel, actions, name);
        fsfree(name);
        for (int bg = 0; bg < 16; bg++) {
            char *name = charstr_printf("color%02d%02d", fg, bg);
            add_color_action(channel, actions, name);
            fsfree(name);
        }
    }
}

static void add_channel_actions(channel_t *channel, GActionGroup *actions)
{
    static GActionEntry win_entries[] = {
        { "bold", bold_activated },
        { "italic", italic_activated },
        { "underline", underline_activated },
        { "original", original_activated },
        { "hide", hide_activated },
        { NULL }
    };
    g_action_map_add_action_entries(G_ACTION_MAP(actions),
                                    win_entries, -1, channel);
    add_color_actions(channel, actions);
    GSimpleAction *autojoin =
        g_simple_action_new_stateful("autojoin", NULL,
                                     g_variant_new_boolean(channel->autojoin));
    g_signal_connect(autojoin, "change-state",
                     G_CALLBACK(autojoin_changed), channel);
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(autojoin));
}

void add_window_actions(GtkWidget *window, channel_t *channel)
{
    static GActionEntry win_entries[] = {
        { "close", close_activated },
        { NULL }
    };
    GActionGroup *actions = (GActionGroup *) g_simple_action_group_new();
    g_action_map_add_action_entries(G_ACTION_MAP(actions),
                                    win_entries, -1, window);
    if (channel)
        add_channel_actions(channel, actions);
    gtk_widget_insert_action_group(window, "win", actions);
}

GtkWidget *build_chat_log(GtkWidget **view, GtkTextMark **end_mark)
{
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    *view = build_passive_text_view();
    gtk_container_add(GTK_CONTAINER(sw), *view);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(*view));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    *end_mark = gtk_text_buffer_create_mark(buffer, "end", &end, FALSE);
    return sw;
}

void furnish_channel(channel_t *channel)
{
    if (channel->window) {
        gtk_window_present(GTK_WINDOW(channel->window));
        return;
    }
    app_t *app = channel->app;
    channel->window = gtk_application_window_new(app->gui.gapp);
    gtk_window_set_icon(GTK_WINDOW(channel->window), app->gui.icon);
    /* TODO: sanitize name */
    char *window_name = charstr_printf("%s: %s", APP_NAME, channel->name);
    gtk_window_set_title(GTK_WINDOW(channel->window), window_name);
    fsfree(window_name);
    add_window_actions(channel->window, channel);
    gtk_window_set_default_size(GTK_WINDOW(channel->window),
                                app->gui.default_width,
                                app->gui.default_height);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *log =
        build_chat_log(&channel->chat_view, &channel->end_of_chat_view);
    gtk_box_pack_start(GTK_BOX(vbox), log, TRUE, TRUE, 0);
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(vbox), sep, FALSE, FALSE, 0);
    GtkWidget *send_pane = build_send_pane(channel);
    gtk_box_pack_start(GTK_BOX(vbox), send_pane, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(channel->window), vbox);
    gtk_widget_show_all(channel->window);
    g_signal_connect(G_OBJECT(channel->window), "destroy",
                     G_CALLBACK(destroy_channel_window), channel);
    replay_channel(channel);
    gtk_widget_grab_focus(channel->input_view);
}

channel_t *get_channel(app_t *app, const gchar *name)
{
    char *key = lcase_string(name);
    avl_elem_t *ae = avl_tree_get(app->channels, key);
    fsfree(key);
    if (!ae)
        return NULL;
    channel_t *channel = (channel_t *) avl_elem_get_value(ae);
    furnish_channel(channel);
    return channel;
}

void reset_nick(app_t *app, const char *new_nick)
{
    fsfree(app->config.nick);
    app->config.nick = charstr_dupstr(new_nick);
    char *title = charstr_printf("%s@%s", APP_NAME, app->config.nick);
    gtk_window_set_title(GTK_WINDOW(app->gui.app_window), title);
    fsfree(title);
    save_session(app);
}
