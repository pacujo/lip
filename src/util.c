#include <fcntl.h>
#include <sys/stat.h>
#include <assert.h>
#include <encjson.h>
#include <fsdyn/charstr.h>
#include "util.h"

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

bool begin_console_line(app_t *app, GtkTextBuffer **console)
{
    bool at_bottom = is_console_at_bottom(app);
    *console = get_console(app);
    append_timestamp(&app->gui.timestamp, time(NULL), *console);
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

void play_message(channel_t *channel, time_t t, const char *from,
                  const char *tag_name, const char *text)
{
    GtkTextBuffer *chat_buffer =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(channel->chat_view));
    update_cursor(chat_buffer);
    append_timestamp(&channel->timestamp, t, chat_buffer);
    if (from) {
        append_text(chat_buffer, from, NULL);
        append_text(chat_buffer, ">", NULL);
    }
    append_text(chat_buffer, text, tag_name);
    append_text(chat_buffer, "\n", NULL);
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(chat_buffer, &end);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(channel->chat_view), &end,
                                 0.05, TRUE, 0.0, 1.0);
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
    json_add_to_object(message, "from", json_make_string(from));
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
    if (app->opts.reset_state || !app->opts.state_file)
        return;
    FILE *statef = fopen(app->opts.state_file, "r");
    if (!statef)
        return;
    json_thing_t *cfg = json_utf8_decode_file(statef, 1000000);
    fclose(statef);
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
    if (!app->opts.state_file)
        return;
    make_parent_dirs(app->opts.state_file);
    FILE *statef = fopen(app->opts.state_file, "w");
    if (!statef) {
        fprintf(stderr, PROGRAM ": cannot open %s\n", app->opts.state_file);
        return;
    }
    json_thing_t *cfg = build_settings(app);
    json_utf8_dump(cfg, statef);
    json_destroy_thing(cfg);
    fclose(statef);
}

void set_autojoin(app_t *app, const char *name, bool enabled)
{
    char *key = name_to_key(name);
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

char *name_to_key(const char *name)
{
    char *key = charstr_dupstr(name);
    for (char *s = key; *s; s++)
        *s = scandinavian_lcase(*s);
    return key;
}
