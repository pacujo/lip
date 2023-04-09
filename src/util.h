#pragma once

#include "lip.h"

extern const char *TIMESTAMP_PATTERN;
bool begin_console_line(app_t *app, GtkTextBuffer **console);
void console_scroll_maybe(app_t *app, bool scroll);
void append_text(GtkTextBuffer *chat_buffer, const gchar *text,
                 const gchar *tag_name);
void append_message(channel_t *channel, const gchar *from,
                    const gchar *tag_name, const gchar *format, ...);

bool valid_server(const char *address);
bool valid_tcp_port(const char *port, int *number);
bool valid_nick(const char *nick);
bool valid_name(const char *name);

void logged_command(app_t *app, const char *prefix, const char *command,
                    list_t *params);
