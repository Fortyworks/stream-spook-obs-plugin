/*
 * StreamSpook for OBS
 * Copyright (C) 2026 Fortyworks
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version. See LICENSE for details.
 */
/*
 * ログの接頭辞と版。PLUGIN_NAME / PLUGIN_VERSION は buildspec.json から
 * CMake が plugin-support.c.in に埋める（obs-plugintemplate の流儀）。
 */
#pragma once

#include <stdarg.h>

extern const char *PLUGIN_NAME;
extern const char *PLUGIN_VERSION;

/* blog() に "[stream-spook] " を頭に付けて流す */
void obs_log(int log_level, const char *format, ...);
extern void blogva(int log_level, const char *format, va_list args);
