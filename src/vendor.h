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
 * obs-websocket の vendor（名前 "stream-spook"）を 1 つだけ持つ。
 *
 * 同じ名前の vendor は 1 度しか登録できないので、音の取り口（spectrum.c）と
 * スティンガー（stinger-transition.c）が別々に登録しに行くと、後から来たほうが
 * 黙って要求を失う。登録はここで 1 回だけして、両方がこの 1 本に要求を足す。
 */
#pragma once

#include "obs-websocket-api.h"

/* obs_module_post_load から、各機能の init より先に 1 回だけ呼ぶ。
 * obs-websocket が無ければ NULL のまま（各機能は vendor 無しで動く） */
void ss_vendor_init(void);

/* 登録した vendor。obs-websocket が無ければ NULL */
obs_websocket_vendor ss_vendor(void);
