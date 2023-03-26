#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include <gtk/gtk.h>
#include <glib-unix.h>

#include <async/async.h>
#include <async/tcp_client.h>
#include <async/queuestream.h>
#include <async/tls_connection.h>
#include <fsdyn/charstr.h>
#include <fsdyn/list.h>
#include <fstrace.h>

typedef enum {
    CONNECTING,
    READY,
    ZOMBIE,
} state_t;

typedef struct {
    async_t *async;
    uint64_t next_timeout;
    state_t state;
    tcp_client_t *client;
    tcp_conn_t *tcp_conn;
    tls_conn_t *tls_conn;
    queuestream_t *outq;
    bytestream_1 input;
    char input_buffer[512];
    char *input_cursor, *input_end;
    struct {
        GtkApplication *gapp;
        GtkTextBuffer *chat_buffer;
    } gui;
} app_t;

static const char *trace_state(void *p)
{
    switch (*(state_t *) p) {
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
        if (!(charstr_char_class(*++p) & CHARSTR_DIGIT) ||
            !(charstr_char_class(*++p) & CHARSTR_DIGIT) ||
            *++p != ' ')
            return NULL;
        return skip_space(p + 1);
    }
    if (!(charstr_char_class(*p++) & CHARSTR_ALPHA))
        return NULL;
    while (charstr_char_class(*p) & CHARSTR_ALPHA)
        p++;
    if (*p++ != ' ')
        return NULL;
    return p;
}

static bool do_it(const char *prefix, const char *command, list_t *params)
{
    // PASS <password>
    // NICK <nick> [ <hopcount> ]
    // USER <user> <host> <server> <realname>
    // OPER <user> <password>
    // QUIT [ <message> ]
    // JOIN <comma-s-channels> [ <comma-s-keys> ]
    // PART <comma-s-channels>
    // MODE <channel> {[+|-]|o|p|s|i|t|n|b|v} [<limit>] [<user>] [<ban mask>]
    // MODE <nickname> {[+|-]|i|w|s|o}
    // TOPIC <channel> [<topic>]
    // NAMES <comma-s-channels>
    // LIST [<comma-s-channels> [<server>]]
    // INVITE <nick> <channel>
    // KICK <channel> <user> [<comment>]
    // VERSION [<server>]
    // STATS [<query> [<server>]]
    // LINKS [[<server>] <mask>]
    // TIME [<server>]
    // ADMIN [<server>]
    // INFO [<server>]
    // PRIVMSG <comma-s-receivers> <text>
    // NOTICE <nick> <text>
    // WHO [<name> [<o>]]
    // WHOIS [<server>] <comma-s-masks>
    // WHOWAS <nick> [<count> [<server>]]
    // PING <server1> [<server2>]
    // PONG <daemon1> [<daemon2>]
    // AWAY [<message>]
    // REHASH
    // USERS [<server>]
    //...;
    return true;
}

FSTRACE_DECL(IRC_ACT_ON, "MSG=%A");
FSTRACE_DECL(IRC_ACT_ON_BAD_COMMAND, "");
FSTRACE_DECL(IRC_ACT_ON_EMPTY_PARAM, "");

static bool act_on_message(const char *cmd, size_t size)
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
    bool result = do_it(prefix, command, params);
    destroy_list(params);
    return result;
}

static void quit(app_t *app)
{
    set_state(app, ZOMBIE);
    async_quit_loop(app->async);
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
                if (!act_on_message(base, app->input_cursor - 1 - base)) {
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
    app->tls_conn = open_tls_client_2(app->async,
                                    tcp_get_input_stream(app->tcp_conn),
                                    TLS_SYSTEM_CA_BUNDLE, "irc.oftc.net");
    tcp_set_output_stream(app->tcp_conn,
                          tls_get_encrypted_output_stream(app->tls_conn));
    app->outq = make_queuestream(app->async);
    tls_set_plain_output_stream(app->tls_conn,
                                queuestream_as_bytestream_1(app->outq));
    app->input = tls_get_plain_input_stream(app->tls_conn);
    action_1 receive_cb = { app, (act_1) receive };
    bytestream_1_register_callback(app->input, receive_cb);
    async_execute(app->async, receive_cb);
    printf("yes\n");
}

static gboolean timeout(gpointer user_data);

static bool do_poll(app_t *app)
{
    uint64_t next_timeout;
    if (async_poll(app->async, &next_timeout) < 0)
        return false;           // wtf?
    if (next_timeout >= app->next_timeout)
        return true;
    uint64_t now = async_now(app->async);
    if (next_timeout < now)
        next_timeout = now;
    app->next_timeout = next_timeout;
    guint interval =
        (next_timeout - async_now(app->async) + ASYNC_MS - 1) / ASYNC_MS;
    g_timeout_add(interval, timeout, app);
    return true;
}

static gboolean timeout(gpointer user_data)
{
    app_t *app = user_data;
    app->next_timeout = (uint64_t) -1;
    do_poll(app);
    return FALSE;
}

static gboolean poll_async(gint fd, GIOCondition condition, gpointer user_data)
{
    return do_poll(user_data) ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static void init_tracing(app_t *app)
{
    fstrace_t *trace = fstrace_direct(stderr);
    fstrace_declare_globals(trace);
    fstrace_select_regex(trace, "^IRC-", NULL);
}

static void connect_to_irc_server(app_t *app)
{
#if 0
    app->client = open_tcp_client(app->async, "irc.oftc.net", 6697);
    action_1 establish_cb = { app, (act_1) establish };
    tcp_client_register_callback(app->client, establish_cb);
    async_execute(app->async, establish_cb);
    app->state = CONNECTING;
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
    do_poll(app);
}

static double get_pixel_width(GdkRectangle *geometry)
{
    GdkMonitor *monitor =
        gdk_display_get_primary_monitor(gdk_display_get_default());
    gdk_monitor_get_workarea(monitor, geometry);
    return gdk_monitor_get_width_mm(monitor) * 0.001 / 
        (geometry->width - geometry->x);
}

static gboolean ignore_key_press(GtkWidget *, GdkEventKey *, app_t *)
{
    return TRUE;
}

static GtkWidget *build_passive_text_view(app_t *app)
{
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
    g_signal_connect(G_OBJECT(view), "key_press_event",
                     G_CALLBACK(ignore_key_press), app);
    return view;
}

static GtkWidget *build_chat_log(app_t *app)
{
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    GtkWidget *view = build_passive_text_view(app);
    app->gui.chat_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    gtk_text_buffer_create_tag(app->gui.chat_buffer,
                               "mine", "foreground", "green", NULL);
    gtk_container_add(GTK_CONTAINER(sw), view);
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

static void update_cursor(GtkTextBuffer *chat_buffer)
{
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(chat_buffer, &end);
    gtk_text_buffer_place_cursor(chat_buffer, &end);
}

static void append_text(GtkTextBuffer *chat_buffer, const gchar *text,
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

static gboolean on_key_press(GtkWidget *view, GdkEventKey *event, app_t *app)
{
    //printf("### %x\n", (unsigned) event->keyval);
    if (!is_enter_key(event))
        return FALSE;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    gchar *text = extract_text(buffer);
    GtkTextBuffer *chat_buffer = app->gui.chat_buffer;
    update_cursor(chat_buffer);
    append_text(chat_buffer, ">", NULL);
    append_text(chat_buffer, text, "mine");
    append_text(chat_buffer, "\n", NULL);
    g_free(text);
    return TRUE;
}

static GtkWidget *build_prompt(app_t *app)
{
    GtkWidget *view = build_passive_text_view(app);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    gtk_text_buffer_set_text(buffer, "⇨", -1);
    return view;
}

static GtkWidget *build_send_pane(app_t *app)
{
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD);
    g_signal_connect(G_OBJECT(view), "key_press_event",
                     G_CALLBACK(on_key_press), app);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sw), view);
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(hbox), build_prompt(app), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), sw, TRUE, TRUE, 0);
    return hbox;
}

static void build_gui(app_t *app)
{
    GdkRectangle geometry;
    double pixel_width = get_pixel_width(&geometry);
    (void) pixel_width;
    GtkWidget *window = gtk_application_window_new(app->gui.gapp);
    gtk_window_set_title(GTK_WINDOW(window), "Besmirch");
    gtk_window_set_default_size(GTK_WINDOW(window),
                                geometry.width / 2, geometry.height * 5 / 6);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(vbox), build_chat_log(app), TRUE, TRUE, 0);
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(vbox), sep, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), build_send_pane(app), FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(window), vbox);
    gtk_widget_show_all(window);
}

FSTRACE_DECL(IRC_INITIALIZE, "");

static void initialize(GtkApplication *, app_t *app)
{
    init_tracing(app);
    FSTRACE(IRC_INITIALIZE);
    connect_to_irc_server(app);
    attach_async_to_gtk(app);
    build_gui(app);
}

int main(int argc, char **argv)
{
    app_t app = {
        .gui = {
            .gapp = gtk_application_new("net.pacujo.irc",
                                        G_APPLICATION_FLAGS_NONE),
        },
        .async = make_async(),
        .next_timeout = (uint64_t) -1,
    };
    g_signal_connect(app.gui.gapp, "activate", G_CALLBACK(initialize), &app);
    int status = g_application_run(G_APPLICATION(app.gui.gapp), argc, argv);
    destroy_async(app.async);
    g_object_unref(app.gui.gapp);
    return status;
}
