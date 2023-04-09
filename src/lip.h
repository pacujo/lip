#pragma once

#include <time.h>

#include <gtk/gtk.h>

#include <async/async.h>
#include <async/tcp_client.h>
#include <async/tls_connection.h>
#include <async/queuestream.h>
#include <fsdyn/avltree.h>
#include <fsdyn/hashtable.h>

#define PROGRAM "lip"
#define APP_NAME "Lip"

typedef enum {
    CONFIGURING,
    CONNECTING,
    READY,
    ZOMBIE,
} state_t;

typedef struct {
    char *key, *name;
} channel_id_t;

typedef struct {
    struct {
        char *trace_include, *trace_exclude;
        char *state_file;    /* NULL, absolute or relative to $HOME */
        bool reset_state;
    } opts;
    struct {
        char *nick, *name, *server;
        int port;
        bool use_tls;
        avl_tree_t *autojoins;  /* of channel_id_t */
    } config;
    int home_dir_fd;
    async_t *async;
    state_t state;
    tcp_client_t *client;
    tcp_conn_t *tcp_conn;
    tls_conn_t *tls_conn;
    queuestream_t *outq;
    bytestream_1 input;
    char input_buffer[512];
    char *input_cursor, *input_end;
    hash_table_t *channels;           /* of key -> channel_t */
    list_t *message_log;              /* of json_thing_t */
    size_t message_log_size;
    struct {
        GtkApplication *gapp;
        gint default_width, default_height;
        double pixel_width;
        GtkWidget *configuration_window;
        GtkWidget *configuration_nick;
        GtkWidget *configuration_name;
        GtkWidget *configuration_server;
        GtkWidget *configuration_port;
        GtkWidget *configuration_use_tls;
        GtkWidget *configuration_autojoins;
        GtkWidget *app_window;
        GtkWidget *scrolled_window;;
        GtkWidget *console;
        struct tm timestamp;
        GtkWidget *join_dialog;
        GtkWidget *join_channel;
    } gui;
} app_t;

typedef struct {
    app_t *app;
    char *key, *name;
    bool autojoin;
    GtkWidget *window;
    GtkWidget *chat_view;
    struct tm timestamp;
} channel_t;

void emit(app_t *app, const char *text);
channel_t *open_channel(app_t *app, const gchar *name, unsigned limit,
                        bool autojoin);
