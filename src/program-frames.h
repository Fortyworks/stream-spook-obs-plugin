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
/* 配信に出て行く絵を小さな JPEG にして、obs-websocket 経由でアプリへ渡す（program-frames.c） */
#pragma once

#include <stdbool.h>

/* obs_module_post_load から呼ぶ（obs-websocket が先に読み込まれている必要がある） */
void program_frames_init(void);

/* obs_module_unload から呼ぶ */
void program_frames_shutdown(void);

/* 要求を足せたか（host_info の features.frames） */
bool program_frames_available(void);
