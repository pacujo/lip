#pragma once

#include "lip.h"

extern const char *TIMESTAMP_PATTERN;
int one_em();
int one_ex();
bool begin_console_line(app_t *app, GtkTextBuffer **console);
void console_scroll_maybe(app_t *app, bool scroll);
char *escape_xml(const char *text);
void append_text(GtkTextBuffer *chat_buffer, const gchar *text,
                 const gchar *tag_name);
void play_message(channel_t *channel, time_t t, const char *from,
                  const char *tag_name, const char *text);
void append_message(channel_t *channel, const gchar *from,
                    const gchar *tag_name, const gchar *format, ...);

bool valid_server(const char *address);
bool valid_tcp_port(const char *port, int *number);
bool valid_nick(const char *nick);
bool valid_name(const char *name);

void logged_command(app_t *app, const char *prefix, const char *command,
                    list_t *params);

void destroy_channel_id(channel_id_t *chid);
void clear_autojoins(app_t *app);
void load_session(app_t *app);
void save_session(app_t *app);
void make_parent_dirs(const char *pathname);
void set_autojoin(app_t *app, const char *name, bool enabled);
char *lcase_string(const char *name);
GtkWidget *build_passive_text_view();
bool is_enter_key(GdkEventKey *event);
void modal_error_dialog(GtkWidget *parent, const gchar *text);
char *highlight(channel_t *channel, const char *text);
char *read_file(const char *pathname, size_t *count);
void add_window_actions(GtkWidget *window, channel_t *channel);
GtkWidget *build_chat_log(GtkWidget **view, GtkTextMark **end_mark);
void furnish_channel(channel_t *channel);
channel_t *get_channel(app_t *app, const gchar *name);
void reset_nick(app_t *app, const char *new_nick);
