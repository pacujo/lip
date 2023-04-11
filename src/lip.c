#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <assert.h>

#include <glib-unix.h>

#include <async/stringstream.h>
#include <fsdyn/charstr.h>
#include <fsdyn/list.h>
#include <fstrace.h>
#include <encjson.h>

#include "lip.h"
#include "ind.h"
#include "rpl.h"
#include "util.h"

static const char *const APPLICATION_ID = "net.pacujo.lip";

static GtkWidget *ensure_main_window(app_t *app);

static const char *trace_state(void *p)
{
    switch (*(state_t *) p) {
        case CONFIGURING:
            return "CONFIGURING";
        case CONNECTING:
            return "CONNECTING";
        case READY:
            return "READY";
        case ZOMBIE:
            return "ZOMBIE";
        default:
            return "?";
    }
}

FSTRACE_DECL(IRC_SET_STATE, "OLD=%I NEW=%I");

static void set_state(app_t *app, state_t state)
{
    FSTRACE(IRC_SET_STATE, trace_state, &app->state, trace_state, &state);
    app->state = state;
}

static char *find_space(char *p)
{
    while (*p && *p != ' ')
        p++;
    return p;
}

static char *skip_space(char *p)
{
    while (*p == ' ')
        p++;
    return p;
}

static char *split_off(char *p)
{
    p = find_space(p);
    char *q = skip_space(p);
    *p = '\0';
    return q;
}

static char *parse_prefix(char *p, const char **prefix)
{
    if (*p != ':') {
        *prefix = NULL;
        return p;
    }
    *prefix = ++p;
    return split_off(p);
}

/* May return NULL. */
static char *parse_command(char *p, const char **command)
{
    *command = p;
    if (charstr_char_class(*p) & CHARSTR_DIGIT) {
        p++;
        if (!(charstr_char_class(*p++) & CHARSTR_DIGIT) ||
            !(charstr_char_class(*p++) & CHARSTR_DIGIT))
            return NULL;
    } else {
        if (!(charstr_char_class(*p++) & CHARSTR_ALPHA))
            return NULL;
        while (charstr_char_class(*p) & CHARSTR_ALPHA)
            p++;
    }
    switch (*p) {
        case '\0':
            return p;
        case ' ':
            *p = '\0';
            return skip_space(p + 1);
        default:
            return NULL;
    }
}

void emit(app_t *app, const char *text)
{
    stringstream_t *sstr = copy_stringstream(app->async, text);
    queuestream_enqueue(app->outq, stringstream_as_bytestream_1(sstr));
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

static bool do_it(app_t *app, const char *prefix, const char *command,
                  list_t *params)
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
 PART <comma-s-channels>
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
    else if (!strcmp(command, "PRIVMSG"))
        done = privmsg(app, prefix, params);
    else if (!strcmp(command, "PING"))
        done = ping(app, prefix, params);
    if (!done)
        dump_message(app, prefix, command, params);
    return true;
}

FSTRACE_DECL(IRC_ACT_ON, "MSG=%A");
FSTRACE_DECL(IRC_ACT_ON_BAD_COMMAND, "");
FSTRACE_DECL(IRC_ACT_ON_EMPTY_PARAM, "");

static bool act_on_message(app_t *app, const char *cmd, size_t size)
{
    FSTRACE(IRC_ACT_ON, cmd, size);
    char buf[size + 1];
    memcpy(buf, cmd, size);
    buf[size] = '\0';
    char *p = buf;
    const char *prefix;
    p = parse_prefix(p, &prefix);
    const char *command;
    p = parse_command(p, &command);
    if (!p) {
        FSTRACE(IRC_ACT_ON_BAD_COMMAND);
        return false;
    }
    list_t *params = make_list();
    for (; *p != '\0' && *p != ':' && *p != ' '; p = split_off(p))
        list_append(params, p);
    switch (*p) {
        case '\0':
            break;
        case ':':
            list_append(params, ++p);
            break;
        default:
            FSTRACE(IRC_ACT_ON_EMPTY_PARAM);
            return false;
    }
    bool result = do_it(app, prefix, command, params);
    destroy_list(params);
    return result;
}

static void quit(app_t *app)
{
    if (app->state == ZOMBIE)
        return;
    set_state(app, ZOMBIE);
    async_quit_loop(app->async);
    g_application_quit(G_APPLICATION(app->gui.gapp));
}

FSTRACE_DECL(IRC_RECEIVE_FAIL, "ERR=%e");
FSTRACE_DECL(IRC_RECEIVE_SPURIOUS, "");
FSTRACE_DECL(IRC_RECEIVE_NUL, "");
FSTRACE_DECL(IRC_RECEIVE_FAILED_ACT, "");
FSTRACE_DECL(IRC_RECEIVE_AGAIN, "");
FSTRACE_DECL(IRC_RECEIVE, "");
FSTRACE_DECL(IRC_DISCONNECTED, "");
FSTRACE_DECL(IRC_RECEIVED, "DATA=%A");
FSTRACE_DECL(IRC_RECEIVE_OVERFLOW, "");

static void receive(app_t *app)
{
    if (app->state != READY) {
        FSTRACE(IRC_RECEIVE_SPURIOUS);
        return;
    }
    for (;;) {
        ssize_t count =
            bytestream_1_read(app->input, app->input_cursor,
                              app->input_end - app->input_cursor);
        if (count < 0) {
            if (errno != EAGAIN) {
                FSTRACE(IRC_RECEIVE_FAIL);
                quit(app);
                return;
            }
            FSTRACE(IRC_RECEIVE_AGAIN);
            assert(errno == EAGAIN);
            return;
        }
        if (count == 0) {
            FSTRACE(IRC_DISCONNECTED);
            quit(app);
            return;
        }
        FSTRACE(IRC_RECEIVED, app->input_cursor, count);
        char *base = app->input_buffer;
        for (; count--; app->input_cursor++) {
            if (!*app->input_cursor) {
                FSTRACE(IRC_RECEIVE_NUL);
                quit(app);
                return;
            }
            if (*app->input_cursor == '\n' &&
                app->input_cursor != base && app->input_cursor[-1] == '\r') {
                if (!act_on_message(app, base, app->input_cursor - 1 - base)) {
                    FSTRACE(IRC_RECEIVE_FAILED_ACT);
                    quit(app);
                    return;
                }
                base = app->input_cursor + 1;
            }
        }
        size_t tail_size = app->input_cursor - base;
        memmove(app->input_buffer, base, tail_size);
        app->input_cursor = app->input_buffer + tail_size;
        if (app->input_cursor == app->input_end) {
            FSTRACE(IRC_RECEIVE_OVERFLOW);
            quit(app);
            return;
        }
    }
}

static void log_in(app_t *app)
{
    emit(app, "NICK ");
    emit(app, app->config.nick);
    emit(app, " \r\n");
    emit(app, "USER ");
    emit(app, app->config.nick);
    emit(app, " * * :");
    emit(app, app->config.name);
    emit(app, "\r\n");
}

static void join_channel(app_t *app, const char *name, bool autojoin)
{
    channel_t *channel = open_channel(app, name, UINT_MAX, autojoin);
    if (valid_nick(channel->name))
        return;
    emit(app, "JOIN ");
    emit(app, channel->name);
    emit(app, "\r\n");
}

static void autojoin_channels(app_t *app)
{
    for (avl_elem_t *ae = avl_tree_get_first(app->config.autojoins); ae;
         ae = avl_tree_next(ae)) {
        channel_id_t *chid = (channel_id_t *) avl_elem_get_value(ae);
        join_channel(app, chid->name, true);
    }
}

FSTRACE_DECL(IRC_ESTABLISH_FAIL, "ERR=%e");
FSTRACE_DECL(IRC_ESTABLISH_SPURIOUS, "");
FSTRACE_DECL(IRC_ESTABLISH_AGAIN, "");
FSTRACE_DECL(IRC_ESTABLISHED, "");

static void establish(app_t *app)
{
    if (app->state != CONNECTING) {
        FSTRACE(IRC_ESTABLISH_SPURIOUS);
        return;
    }
    app->tcp_conn = tcp_client_establish(app->client);
    if (!app->tcp_conn) {
        if (errno != EAGAIN) {
            FSTRACE(IRC_ESTABLISH_FAIL);
            set_state(app, ZOMBIE);
            async_quit_loop(app->async);
            return;
        }
        FSTRACE(IRC_ESTABLISH_AGAIN);
        return;
    }
    FSTRACE(IRC_ESTABLISHED);
    tcp_client_close(app->client);
    set_state(app, READY);
    app->input_cursor = app->input_buffer;
    app->input_end = app->input_buffer + sizeof app->input_buffer;
    app->outq = make_queuestream(app->async);
    bytestream_1 plain_output = queuestream_as_bytestream_1(app->outq);
    bytestream_1 tcp_input = tcp_get_input_stream(app->tcp_conn);
    if (app->config.use_tls) {
        app->tls_conn =
            open_tls_client_2(app->async, tcp_input, TLS_SYSTEM_CA_BUNDLE,
                              app->config.server);
        tcp_set_output_stream(app->tcp_conn,
                              tls_get_encrypted_output_stream(app->tls_conn));
        tls_set_plain_output_stream(app->tls_conn, plain_output);
        app->input = tls_get_plain_input_stream(app->tls_conn);
    } else {
        app->tls_conn = NULL;
        tcp_set_output_stream(app->tcp_conn, plain_output);
        app->input = tcp_input;
    }
    action_1 receive_cb = { app, (act_1) receive };
    bytestream_1_register_callback(app->input, receive_cb);
    async_execute(app->async, receive_cb);
    log_in(app);
    autojoin_channels(app);
}

FSTRACE_DECL(IRC_POLL_ASYNC, "");

static gboolean poll_async(gint fd, GIOCondition condition, app_t *app)
{
    FSTRACE(IRC_POLL_ASYNC);
    return async_poll_2(app->async) >= 0 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static void init_tracing(app_t *app)
{
    fstrace_t *trace = fstrace_direct(stderr);
    fstrace_declare_globals(trace);
    fstrace_select_regex(trace, app->opts.trace_include,
                         app->opts.trace_exclude);
}

static void connect_to_irc_server(app_t *app)
{
#if 1
    app->client =
        open_tcp_client(app->async, app->config.server, app->config.port);
    action_1 establish_cb = { app, (act_1) establish };
    tcp_client_register_callback(app->client, establish_cb);
    async_execute(app->async, establish_cb);
    set_state(app, CONNECTING);
#else
    (void) establish;
    app->state = ZOMBIE;
#endif
}

static void attach_async_to_gtk(app_t *app)
{
    GSource *source = g_unix_fd_source_new(async_fd(app->async), G_IO_IN);
    g_source_set_callback(source, G_SOURCE_FUNC(poll_async), app, NULL);
    g_source_attach(source, NULL);
    async_poll_2(app->async);
}

static double get_pixel_width(GdkRectangle *geometry)
{
    GdkMonitor *monitor =
        gdk_display_get_primary_monitor(gdk_display_get_default());
    gdk_monitor_get_workarea(monitor, geometry);
    return gdk_monitor_get_width_mm(monitor) * 0.001 / 
        (geometry->width - geometry->x);
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

static int one_em()
{
    static int em = -1;
    if (em < 0)
        text_dimensions("m", &em, NULL);
    return em;
}

static int one_ex()
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

static GtkWidget *build_passive_text_view()
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

static GtkWidget *build_chat_log(GtkWidget **view)
{
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    *view = build_passive_text_view();
    GtkTextBuffer *chat_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(*view));
    gtk_text_buffer_create_tag(chat_buffer,
                               "mine", "foreground", "green", NULL);
    gtk_text_buffer_create_tag(chat_buffer,
                               "theirs", "foreground", "red", NULL);
    gtk_text_buffer_create_tag(chat_buffer,
                               "log", "foreground", "cyan", NULL);
    gtk_text_buffer_create_tag(chat_buffer,
                               "error", "foreground", "red", NULL);
    gtk_container_add(GTK_CONTAINER(sw), *view);
    return sw;
}

static bool is_enter_key(GdkEventKey *event)
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
    gchar *text = gtk_text_buffer_get_text(buffer, &start, &end, TRUE);
    gtk_text_buffer_set_text(buffer, "", -1);
    return text;
}

static void send_message(channel_t *channel, const gchar *text)
{
    app_t *app = channel->app;
    emit(app, "PRIVMSG ");
    emit(app, channel->name);
    emit(app, " :");
    emit(app, text);
    emit(app, "\r\n");
}

static gboolean on_key_press(GtkWidget *view, GdkEventKey *event,
                             channel_t *channel)
{
    if (!is_enter_key(event))
        return FALSE;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    gchar *text = extract_text(buffer);
    append_message(channel, channel->app->config.nick, "mine", "%s", text);
    send_message(channel, text);
    g_free(text);
    return TRUE;
}

static GtkWidget *build_prompt(channel_t *channel)
{
    GtkWidget *view = build_passive_text_view(channel->app);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    gtk_text_buffer_set_text(buffer, "⇨", -1);
    return view;
}

static GtkWidget *build_send_pane(channel_t *channel)
{
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD);
    g_signal_connect(G_OBJECT(view), "key_press_event",
                     G_CALLBACK(on_key_press), channel);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sw), view);
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(hbox), build_prompt(channel), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), sw, TRUE, TRUE, 0);
    return hbox;
}

static void quit_activated(GSimpleAction *action, GVariant *parameter,
                           gpointer user_data)
{
    app_t *app = user_data;
    quit(app);
}

static void add_margin(GtkWidget *widget)
{
    int margin = one_ex() / 2;
    gtk_widget_set_margin_top(widget, margin);
    gtk_widget_set_margin_bottom(widget, margin);
    gtk_widget_set_margin_start(widget, margin);
    gtk_widget_set_margin_end(widget, margin);
}

static bool valid_channel_name(const char *name)
{
    if (strlen(name) > 50)
        return false;
    const char *p = name;
    switch (*p) {
        case '&':
        case '#':
        case '+':
        case '!':
            break;
        default:
            return false;
    }
    for (;;)
        switch (*++p) {
            case '\0':
                return true;
            case ' ':
            case '\7':
            case ',':
            case '\r':
            case '\n':
                return false;
            default:
                ;
        }
}

static void modal_error_dialog(GtkWidget *parent, const gchar *text)
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

static void close_activated(GSimpleAction *action, GVariant *parameter,
                            gpointer user_data)
{
    GtkWidget *window = user_data;
    gtk_window_close(GTK_WINDOW(window));
}

static void destroy_channel_window(GtkWidget *, channel_t *channel)
{
    destroy_hash_element(hash_table_pop(channel->app->channels, channel->key));
    fsfree(channel->key);
    fsfree(channel->name);
    fsfree(channel);
}

static void autojoin_changed(GSimpleAction *action, GVariant *value,
                             channel_t *channel)
{
    channel->autojoin = g_variant_get_boolean(value);
    g_simple_action_set_state(action, value);
    set_autojoin(channel->app, channel->name, channel->autojoin);
    save_session(channel->app);
}

static void add_channel_actions(channel_t *channel, GActionGroup *actions)
{
    GSimpleAction *autojoin =
        g_simple_action_new_stateful("autojoin", NULL,
                                     g_variant_new_boolean(channel->autojoin));
    g_signal_connect(autojoin, "change-state",
                     G_CALLBACK(autojoin_changed), channel);
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(autojoin));
}

static void add_window_actions(GtkWidget *window, channel_t *channel)
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

static int message_log_filter(const struct dirent *entity)
{
    return charstr_skip_prefix(entity->d_name, "messages") != NULL &&
        charstr_ends_with(entity->d_name, ".log");
}

static int message_log_cmp(const struct dirent **a, const struct dirent **b)
{
    return strcmp((*a)->d_name, (*b)->d_name);
}

static char *read_file(const char *pathname, size_t *count)
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
                    const char *key, *from, *tag, *text;
                    unsigned long long t;
                    if (json_object_get_string(message, "channel", &key) &&
                        !strcmp(key, channel->key) &&
                        json_object_get_unsigned(message, "time", &t) &&
                        json_object_get_string(message, "from", &from) &&
                        json_object_get_string(message, "tag", &tag) &&
                        json_object_get_string(message, "text", &text))
                        play_message(channel, t, from, tag, text);
                    json_destroy_thing(message);
                }
                cursor = p;
            }
        fsfree(content);
    }
    free(namelist);
}

static channel_t *make_channel(app_t *app, char *key, const gchar *name,
                               bool autojoin)
{
    channel_t *channel = fsalloc(sizeof *channel);
    channel->app = app;
    channel->key = key;
    channel->name = charstr_dupstr(name);
    channel->autojoin = autojoin;
    channel->window = gtk_application_window_new(app->gui.gapp);
    time_t t0 = 0;
    localtime_r(&t0, &channel->timestamp);
    /* TODO: sanitize name */
    char *window_name = charstr_printf("%s: %s", APP_NAME, name);
    gtk_window_set_title(GTK_WINDOW(channel->window), window_name);
    fsfree(window_name);
    add_window_actions(channel->window, channel);
    gtk_window_set_default_size(GTK_WINDOW(channel->window),
                                app->gui.default_width,
                                app->gui.default_height);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *log = build_chat_log(&channel->chat_view);
    gtk_box_pack_start(GTK_BOX(vbox), log, TRUE, TRUE, 0);
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(vbox), sep, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), build_send_pane(channel),
                       FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(channel->window), vbox);
    gtk_widget_show_all(channel->window);
    g_signal_connect(G_OBJECT(channel->window), "destroy",
                     G_CALLBACK(destroy_channel_window), channel);
    replay_channel(channel);
    return channel;
}

channel_t *open_channel(app_t *app, const gchar *name, unsigned limit,
                        bool autojoin)
{
    char *key = name_to_key(name);
    hash_elem_t *he = hash_table_get(app->channels, key);
    if (he) {
        fsfree(key);
        channel_t *channel = (channel_t *) hash_elem_get_value(he);
        gtk_window_present(GTK_WINDOW(channel->window));
        return channel;
    }
    if (hash_table_size(app->channels) >= limit)
        return NULL;
    channel_t *channel = make_channel(app, key, name, autojoin);
    hash_table_put(app->channels, channel->key, channel);
    return channel;
}

static void join_ok_response(app_t *app)
{
    const gchar *text = gtk_entry_get_text(GTK_ENTRY(app->gui.join_channel));
    if (!valid_nick(text) && !valid_channel_name(text)) {
        modal_error_dialog(ensure_main_window(app), "Bad nick or channel name");
        return;
    }
    join_channel(app, text, false);
    gtk_widget_destroy(app->gui.join_dialog);
    app->gui.join_dialog = NULL;
}

static void join_cancel_response(app_t *app)
{
    gtk_widget_destroy(app->gui.join_dialog);
    app->gui.join_dialog = NULL;
}

static void join_response(app_t *app, gint response_id)
{
    switch (response_id) {
        case GTK_RESPONSE_OK:
            join_ok_response(app);
            break;
        case GTK_RESPONSE_CANCEL:
            join_cancel_response(app);
            break;
        default:
            abort();
    }
}

static gboolean join_dialog_key_press(GtkWidget *, GdkEventKey *event,
                                      app_t *app)
{
    if (is_enter_key(event)) {
        join_ok_response(app);
        return TRUE;
    }
    if (event->keyval == GDK_KEY_Escape) {
        join_cancel_response(app);
        return TRUE;
    }
    return FALSE;
}

static GtkWidget *entry_cell(GtkWidget *container, const gchar *prompt,
                             const gchar *initial_text)
{
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, one_em());
    add_margin(hbox);
    GtkWidget *label = gtk_label_new(prompt);
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), initial_text);
    gtk_box_pack_start(GTK_BOX(hbox), entry, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(container), hbox);
    return entry;
}

static GtkWidget *checkbox(GtkWidget *container, const gchar *prompt,
                           bool initial_state)
{
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, one_em());
    add_margin(hbox);
    GtkWidget *label = gtk_label_new(prompt);
    gtk_widget_set_halign(label, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(hbox), label, TRUE, TRUE, 0);
    GtkWidget *button = gtk_switch_new();
    gtk_switch_set_state(GTK_SWITCH(button), initial_state);
    gtk_box_pack_start(GTK_BOX(hbox), button, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(container), hbox);
    return button;
}

static void join_activated(GSimpleAction *action, GVariant *parameter,
                           gpointer user_data)
{
    app_t *app = user_data;
    if (app->gui.join_dialog) {
        gtk_window_present(GTK_WINDOW(ensure_main_window(app)));
        return;
    }
    app->gui.join_dialog =
        gtk_dialog_new_with_buttons("Join Channel",
                                    GTK_WINDOW(ensure_main_window(app)),
                                    GTK_DIALOG_DESTROY_WITH_PARENT,
                                    "_Cancel", GTK_RESPONSE_CANCEL,
                                    "_OK", GTK_RESPONSE_OK,
                                    NULL);
    g_signal_connect_swapped(app->gui.join_dialog, "response",
                             G_CALLBACK(join_response), app);
    GtkWidget *content_area =
        gtk_dialog_get_content_area(GTK_DIALOG(app->gui.join_dialog));
    app->gui.join_channel = entry_cell(content_area, "Channel", "");
    g_signal_connect(app->gui.join_dialog, "key_press_event",
                     G_CALLBACK(join_dialog_key_press), app);
    gtk_widget_show_all(app->gui.join_dialog);
}

static void accelerate(app_t *app, const gchar *action, const gchar *accel)
{
    const gchar *accels[] = { accel, NULL };
    gtk_application_set_accels_for_action(GTK_APPLICATION(app->gui.gapp),
                                          action, accels);
}

static void build_menus(app_t *app)
{
    static GActionEntry app_entries[] = {
        { "quit", quit_activated },
        { "join", join_activated },
        { NULL }
    };
    const gchar *menu_xml =
        "<?xml version='1.0'?>"
        "<interface>"
        " <menu id='menubar'>"
        "  <section>"
        "   <submenu>"
        "    <attribute name='label' translatable='yes'>_File</attribute>"
        "    <section>"
        "     <item>"
        "      <attribute name='label' translatable='yes'>_Join...</attribute>"
        "      <attribute name='action'>app.join</attribute>"
        "     </item>"
        "     <item>"
        "      <attribute name='label' translatable='yes'>Autojoin</attribute>"
        "      <attribute name='action'>win.autojoin</attribute>"
        "     </item>"
        "    </section>"
        "    <section>"
        "     <item>"
        "      <attribute name='label' translatable='yes'>_Close</attribute>"
        "      <attribute name='action'>win.close</attribute>"
        "     </item>"
        "     <item>"
        "      <attribute name='label' translatable='yes'>_Quit</attribute>"
        "      <attribute name='action'>app.quit</attribute>"
        "     </item>"
        "    </section>"
        "   </submenu>"
        "  </section>"
        " </menu>"
        "</interface>";
    g_action_map_add_action_entries(G_ACTION_MAP(app->gui.gapp),
                                    app_entries, -1, app);
    accelerate(app, "app.join", "<Ctrl>J");
    accelerate(app, "win.close", "<Ctrl>W");
    accelerate(app, "app.quit", "<Ctrl>Q");
    GtkBuilder *builder = gtk_builder_new_from_string(menu_xml, -1);
    GMenuModel *model =
        G_MENU_MODEL(gtk_builder_get_object(builder, "menubar"));
    gtk_application_set_menubar(app->gui.gapp, model);
    g_clear_object(&builder);
}

static void destroy_main_window(GtkWidget *, app_t *app)
{
    app->gui.app_window = NULL;
}

static GtkWidget *ensure_main_window(app_t *app)
{
    assert(app->state > CONFIGURING);
    if (app->gui.app_window) {
        gtk_window_present(GTK_WINDOW(app->gui.app_window));
        return app->gui.app_window;
    }
    app->gui.app_window = gtk_application_window_new(app->gui.gapp);
    gtk_window_set_title(GTK_WINDOW(app->gui.app_window), APP_NAME);
    add_window_actions(app->gui.app_window, NULL);
    gtk_window_set_default_size(GTK_WINDOW(app->gui.app_window),
                                app->gui.default_width,
                                app->gui.default_height);
    app->gui.scrolled_window = build_chat_log(&app->gui.console);
    gtk_container_add(GTK_CONTAINER(app->gui.app_window),
                      app->gui.scrolled_window);
    gtk_widget_show_all(app->gui.app_window);
    g_signal_connect(G_OBJECT(app->gui.app_window), "destroy",
                     G_CALLBACK(destroy_main_window), app);
    return app->gui.app_window;
}

static void collect_autojoins(app_t *app)
{
    avl_tree_t *old_autojoins = avl_tree_copy(app->config.autojoins);
    while (!avl_tree_empty(app->config.autojoins))
        destroy_avl_element(avl_tree_pop_first(app->config.autojoins));
    GtkWidget *listbox = app->gui.configuration_autojoins;
    for (gint i = 0; !avl_tree_empty(old_autojoins); i++) {
        avl_elem_t *ae = avl_tree_pop_first(old_autojoins);
        channel_id_t *chid = (channel_id_t *) avl_elem_get_value(ae);
        destroy_avl_element(ae);
        GtkListBoxRow *row =
            gtk_list_box_get_row_at_index(GTK_LIST_BOX(listbox), i);
        if (gtk_list_box_row_is_selected(row))
            avl_tree_put(app->config.autojoins, chid->key, chid);
        else destroy_channel_id(chid);
    }
    destroy_avl_tree(old_autojoins);
}

static bool set_up_cache_directory(app_t *app, const gchar *cache_dir)
{
    /* Test access with a dummy seed file. */
    char *seed = charstr_printf("%s/.seed", cache_dir);
    make_parent_dirs(seed);
    FILE *seedf = fopen(seed, "w");
    fsfree(seed);
    if (!seedf)
        return false;
    fclose(seedf);
    enum { CACHE_ROTATE_SIZE = 200000 };
    char *cache_prefix = charstr_printf("%s/messages", cache_dir);
    app->cache_params = (rotatable_params_t) {
        .uid = geteuid(),
        .gid = getegid(),
        .max_files = -1,
        .max_seconds = -1,
        .max_bytes = 2 * 1000 * 1000,
    };
    app->cache =
        make_rotatable(cache_prefix, ".log", CACHE_ROTATE_SIZE,
                       &app->cache_params);
    assert(app->cache);
    fsfree(cache_prefix);
    return true;
}

static void configuration_ok_response(app_t *app)
{
    const gchar *nick =
        gtk_entry_get_text(GTK_ENTRY(app->gui.configuration_nick));
    const gchar *name =
        gtk_entry_get_text(GTK_ENTRY(app->gui.configuration_name));
    const gchar *server =
        gtk_entry_get_text(GTK_ENTRY(app->gui.configuration_server));
    const gchar *port =
        gtk_entry_get_text(GTK_ENTRY(app->gui.configuration_port));
    const gboolean use_tls =
        gtk_switch_get_state(GTK_SWITCH(app->gui.configuration_use_tls));
    const gchar *cache_dir = 
        gtk_entry_get_text(GTK_ENTRY(app->gui.configuration_cache_dir));
    if (!valid_nick(nick)) {
        modal_error_dialog(app->gui.configuration_window, "Bad nick");
        return;
    }
    if (!valid_name(name)) {
        modal_error_dialog(app->gui.configuration_window, "Bad name");
        return;
    }
    if (!valid_server(server)) {
        modal_error_dialog(app->gui.configuration_window, "Bad server host");
        return;
    }
    int port_number;
    if (!valid_tcp_port(port, &port_number)) {
        modal_error_dialog(app->gui.configuration_window,
                           "Bad TCP port number");
        return;
    }
    if (!set_up_cache_directory(app, cache_dir)) {
        modal_error_dialog(app->gui.configuration_window,
                           "Failed to set up cache directory");
        return;
    }
    app->config.nick = charstr_dupstr(nick);
    app->config.name = charstr_dupstr(name);
    app->config.server = charstr_dupstr(server);
    app->config.port = port_number;
    app->config.use_tls = use_tls;
    collect_autojoins(app);
    app->config.cache_directory = charstr_dupstr(cache_dir);
    gtk_widget_destroy(app->gui.configuration_window);
    app->gui.configuration_window = NULL;
    save_session(app);
    set_state(app, CONNECTING);
    ensure_main_window(app);
    connect_to_irc_server(app);
}

static void configuration_cancel_response(app_t *app)
{
    gtk_widget_destroy(app->gui.configuration_window);
    app->gui.configuration_window = NULL;
    quit(app);
}

static void configuration_response(app_t *app, gint response_id)
{
    switch (response_id) {
        case GTK_RESPONSE_OK:
            configuration_ok_response(app);
            break;
        case GTK_RESPONSE_CANCEL:
            configuration_cancel_response(app);
            break;
        default:
            abort();
    }
}

static gboolean configuration_dialog_key_press(GtkWidget *, GdkEventKey *event,
                                               app_t *app)
{
    if (is_enter_key(event)) {
        configuration_ok_response(app);
        return TRUE;
    }
    if (event->keyval == GDK_KEY_Escape) {
        configuration_cancel_response(app);
        return TRUE;
    }
    return FALSE;
}

static void configuration_dialog_change_cache(GtkButton *, app_t *app)
{
    GtkFileChooserNative *dialog =
        gtk_file_chooser_native_new(APP_NAME " Cache Directory",
                                    GTK_WINDOW(app->gui.configuration_window),
                                    GTK_FILE_CHOOSER_ACTION_CREATE_FOLDER,
                                    "_Select",
                                    "_Cancel");
    gtk_file_chooser_set_show_hidden(GTK_FILE_CHOOSER(dialog), TRUE);
    gint response = gtk_native_dialog_run(GTK_NATIVE_DIALOG(dialog));
    if (response == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        gtk_entry_set_text(GTK_ENTRY(app->gui.configuration_cache_dir), path);
        g_free(path);
    }
    g_object_unref(dialog);
}

static void port_gui(app_t *app, GtkWidget *content_area)
{
    GtkWidget *port_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, one_em());
    gtk_container_add(GTK_CONTAINER(content_area), port_row);
    char *port = charstr_printf("%d", app->config.port);
    app->gui.configuration_port = entry_cell(port_row, "TCP Port", port);
    fsfree(port);
    app->gui.configuration_use_tls =
        checkbox(port_row, "Use TLS", app->config.use_tls);
    add_margin(app->gui.configuration_use_tls);
}

static void autojoin_gui(app_t *app, GtkWidget *content_area)
{
    if (avl_tree_empty(app->config.autojoins)) {
        app->gui.configuration_autojoins = NULL;
        return;
    }
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    add_margin(vbox);
    gtk_box_pack_start(GTK_BOX(content_area), vbox, TRUE, TRUE, 0);
    GtkWidget *heading = gtk_label_new("Autojoin Chats/Channels");
    gtk_box_pack_start(GTK_BOX(vbox), heading, FALSE, FALSE, 0);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_box_pack_start(GTK_BOX(vbox), sw, TRUE, TRUE, 0);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    GtkWidget *listbox = app->gui.configuration_autojoins = gtk_list_box_new();
    gtk_container_add(GTK_CONTAINER(sw), listbox);
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(listbox),
                                    GTK_SELECTION_MULTIPLE);
    for (avl_elem_t *ae = avl_tree_get_first(app->config.autojoins); ae;
         ae = avl_tree_next(ae)) {
        channel_id_t *chid = (channel_id_t *) avl_elem_get_value(ae);
        GtkWidget *row = gtk_list_box_row_new();
        gtk_container_add(GTK_CONTAINER(row), gtk_label_new(chid->name));
        gtk_list_box_insert(GTK_LIST_BOX(listbox), row, -1);
        gtk_list_box_select_row(GTK_LIST_BOX(listbox), GTK_LIST_BOX_ROW(row));
    }
}

static void cache_dir_gui(app_t *app, GtkWidget *content_area)
{
    GtkWidget *cache_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, one_em());
    gtk_container_add(GTK_CONTAINER(content_area), cache_row);
    app->gui.configuration_cache_dir = entry_cell(cache_row, "Cache Directory",
                                                  app->config.cache_directory);
    GtkWidget *change_cache = gtk_button_new_with_label("Change...");
    add_margin(change_cache);
    g_signal_connect(change_cache, "clicked",
                     G_CALLBACK(configuration_dialog_change_cache), app);
    gtk_container_add(GTK_CONTAINER(cache_row), change_cache);
}

static void configure(app_t *app)
{
    assert(app->state == CONFIGURING);
    assert(!app->gui.configuration_window);
    load_session(app);
    app->gui.configuration_window = gtk_application_window_new(app->gui.gapp);
    GtkWidget *dialog =
        gtk_dialog_new_with_buttons(APP_NAME ": Configuration",
                                    GTK_WINDOW(app->gui.configuration_window),
                                    GTK_DIALOG_DESTROY_WITH_PARENT,
                                    "_Cancel", GTK_RESPONSE_CANCEL,
                                    "_OK", GTK_RESPONSE_OK,
                                    NULL);
    g_signal_connect_swapped(dialog, "response",
                             G_CALLBACK(configuration_response), app);
    GtkWidget *content_area =
        gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    app->gui.configuration_nick =
        entry_cell(content_area, "Your Nick", app->config.nick);
    app->gui.configuration_name =
        entry_cell(content_area, "Your Name", app->config.name);
    app->gui.configuration_server =
        entry_cell(content_area, "Server Host", app->config.server);
    port_gui(app, content_area);
    autojoin_gui(app, content_area);
    cache_dir_gui(app, content_area);
    g_signal_connect(dialog, "key_press_event",
                     G_CALLBACK(configuration_dialog_key_press), app);
    gtk_widget_show_all(dialog);
}

FSTRACE_DECL(IRC_ACTIVATE, "");

static void activate(GtkApplication *, app_t *app)
{
    init_tracing(app);
    FSTRACE(IRC_ACTIVATE);
    app->async = make_async();
    attach_async_to_gtk(app);
    build_menus(app);
    GdkRectangle geometry;
    app->gui.pixel_width = get_pixel_width(&geometry);
    app->gui.default_width = geometry.width / 2;
    app->gui.default_height = geometry.height * 5 / 6;
    configure(app);
}

FSTRACE_DECL(IRC_SHUT_DOWN, "");

static void shut_down(GtkApplication *, app_t *app)
{
    FSTRACE(IRC_SHUT_DOWN);
    quit(app);
}


FSTRACE_DECL(IRC_COMMAND_OPTIONS, "");

static gint command_options(GtkApplication *, GVariantDict *options, app_t *app)
{
    FSTRACE(IRC_COMMAND_OPTIONS);
    const char *arg;
    if (g_variant_dict_lookup(options, "state", "s", &arg)) {
        fsfree(app->opts.state_file);
        app->opts.state_file = charstr_dupstr(arg);
    }
    if (g_variant_dict_lookup(options, "stateless", "b", NULL)) {
        fsfree(app->opts.state_file);
        app->opts.state_file = NULL;
    }
    app->opts.reset_state =
        g_variant_dict_lookup(options, "reset-state", "b", NULL);
    if (g_variant_dict_lookup(options, "trace-include", "s", &arg)) {
        fsfree(app->opts.trace_include);
        app->opts.trace_include = charstr_dupstr(arg);
    }
    if (g_variant_dict_lookup(options, "trace-exclude", "s", &arg)) {
        fsfree(app->opts.trace_exclude);
        app->opts.trace_exclude = charstr_dupstr(arg);
    }
    return -1;                  /* carry on */
}

static void add_command_options(app_t *app)
{
    g_application_add_main_option(G_APPLICATION(app->gui.gapp),
                                  "state", 's',
                                  G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_STRING,
                                  "State file (absolute or relative to $HOME)",
                                  "PATH");
    g_application_add_main_option(G_APPLICATION(app->gui.gapp),
                                  "stateless", 0,
                                  G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_NONE,
                                  "No state file", NULL);
    g_application_add_main_option(G_APPLICATION(app->gui.gapp),
                                  "reset-state", 0,
                                  G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_NONE,
                                  "Reset state file", NULL);
    g_application_add_main_option(G_APPLICATION(app->gui.gapp),
                                  "trace-include", 0,
                                  G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_STRING,
                                  "Specify trace events", "REGEXP");
    g_application_add_main_option(G_APPLICATION(app->gui.gapp),
                                  "trace-exclude", 0,
                                  G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_STRING,
                                  "Exclude trace events", "REGEXP");
    g_signal_connect(app->gui.gapp, "handle-local-options",
                     G_CALLBACK(command_options), app);
}

int main(int argc, char **argv)
{
    app_t app = {
        .config = {
            .autojoins = make_avl_tree((void *) strcmp),
        },
        .gui = {
            .gapp = gtk_application_new(APPLICATION_ID,
                                        G_APPLICATION_FLAGS_NONE),
        },
        .state = CONFIGURING,
        .channels = make_hash_table(1000, (void *) hash_string,
                                    (void *) strcmp),
    };
    app.home_dir = getenv("HOME");
    if (!app.home_dir || *app.home_dir != '/') {
        fprintf(stderr, PROGRAM ": no HOME in the environment\n");
        return EXIT_FAILURE;
    }
    app.opts.state_file =
        charstr_printf("%s/.config/lip/state.json", app.home_dir);
    g_signal_connect(app.gui.gapp, "activate", G_CALLBACK(activate), &app);
    g_signal_connect(app.gui.gapp, "shutdown", G_CALLBACK(shut_down), &app);
    add_command_options(&app);
    int status = g_application_run(G_APPLICATION(app.gui.gapp), argc, argv);
    if (app.async)
        destroy_async(app.async);
    if (app.cache)
        destroy_rotatable(app.cache);
    /* TODO: destroy channnels */
    /* TODO: disconnect */
    fsfree(app.config.nick);
    fsfree(app.config.name);
    fsfree(app.config.server);
    fsfree(app.config.cache_directory);
    g_clear_object(&app.gui.gapp);
    clear_autojoins(&app);
    destroy_avl_tree(app.config.autojoins);
    fsfree(app.opts.trace_include);
    fsfree(app.opts.trace_exclude);
    fsfree(app.opts.state_file);
    return status;
}
