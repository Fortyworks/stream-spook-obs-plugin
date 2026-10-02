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
/* 入力ソースの音量（メーター）をアプリへ流す（meters.c） */
#pragma once

/* obs_module_post_load から、ss_vendor_init のあとに呼ぶ */
void meters_init(void);

/* obs_module_unload から呼ぶ */
void meters_shutdown(void);
