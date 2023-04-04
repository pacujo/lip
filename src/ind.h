#pragma once

#include "lip.h"

bool join(app_t *app, const char *prefix, list_t *params);
bool mode(app_t *app, const char *prefix, list_t *params);
bool notice(app_t *app, const char *prefix, list_t *params);
bool ping(app_t *app, const char *prefix, list_t *params);
bool privmsg(app_t *app, const char *prefix, list_t *params);
