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
/* シーントランジション「StreamSpook: スティンガー」（stinger-transition.c） */
#pragma once

/* obs_module_load から呼ぶ（トランジションの種類を登録する） */
void stinger_register(void);

/* obs_module_post_load から、ss_vendor_init のあとに呼ぶ */
void stinger_init_vendor(void);

/* obs_module_unload から呼ぶ */
void stinger_shutdown(void);
