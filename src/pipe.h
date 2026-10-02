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
 * Streamlabs Desktop で読み込まれたときにだけ開く、自前の口（名前付きパイプ
 * \\.\pipe\stream-spook）。Streamlabs Desktop には obs-websocket が入っていないので、
 * vendor の要求とイベントをこちらで運ぶ。
 *
 * ─── 話し方 ────────────────────────────────────────────────────────────────
 * 1 行 1 つの JSON（UTF-8、改行 \n で区切る）。中身は vendor の要求と同じ。
 *   要求      { "id": 1, "type": "spectrum_subscribe", "data": { ... } }
 *   応答      { "id": 1, "data": { ... } }
 *             { "id": 1, "error": "unknown_request" }    知らない要求
 *   イベント  { "event": "spectrum", "data": { ... } }
 *
 * つなげるのはこの PC の中からだけ（PIPE_REJECT_REMOTE_CLIENTS）。同時に
 * MAX_CLIENTS 本まで。読まずに溜めているつなぎ手は、書き込みが詰まった時点で切る
 * （音の受け取りを止めないため）。
 */
#pragma once

#include <obs-data.h>
#include <stdbool.h>

#define SS_PIPE_NAME "\\\\.\\pipe\\stream-spook"

/* 開けたら true。Windows 以外では何もせず false */
bool ss_pipe_start(void);

void ss_pipe_stop(void);

/* つながっている全部にイベントを流す。stop のあとに呼ばれても何もしない */
void ss_pipe_emit(const char *type, obs_data_t *data);
