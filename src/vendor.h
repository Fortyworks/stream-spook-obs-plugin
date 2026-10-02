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
 * アプリ本体と話す口を 1 か所にまとめる。
 *
 * 口は 2 本ある。
 *   - obs-websocket の vendor（名前 "stream-spook"）。OBS Studio ではこれだけ
 *   - 自前の名前付きパイプ（pipe.c）。Streamlabs Desktop には obs-websocket が
 *     入っていないので、そちらで読み込まれたときだけ開く
 *
 * 各機能（spectrum.c / meters.c / stinger-transition.c）は口を意識しない。
 * ss_vendor_register_request で要求を足し、ss_vendor_emit でイベントを流すと、
 * 開いている口の全部に届く。要求と応答・イベントの形（obs_data）はどちらの口でも同じ。
 *
 * 同じ名前の vendor は 1 度しか登録できないので、各機能が別々に登録しに行くと、
 * 後から来たほうが黙って要求を失う。登録はここで 1 回だけする。
 */
#pragma once

#include <obs-data.h>
#include <stdbool.h>

#include "obs-websocket-api.h"

/* obs_module_post_load から、各機能の init より先に 1 回だけ呼ぶ */
void ss_vendor_init(void);

/* obs_module_post_load から、各機能が要求を足し終えたあとに呼ぶ。
 * Streamlabs Desktop で読み込まれたときだけパイプを開く */
void ss_vendor_open_pipe(void);

/* obs_module_unload から、各機能の shutdown のあとに呼ぶ */
void ss_vendor_shutdown(void);

/* 口が 1 本でも開いているか。開いていなければ、各機能は要求を足さずに黙る */
bool ss_vendor_available(void);

/* 要求を足す。型は obs-websocket の vendor API と同じ */
void ss_vendor_register_request(const char *type, obs_websocket_request_callback_function cb, void *priv);

/* 開いている口の全部にイベントを流す */
void ss_vendor_emit(const char *type, obs_data_t *data);

/* 足された要求を名前で呼ぶ（パイプから）。知らない名前なら false */
bool ss_vendor_call(const char *type, obs_data_t *req, obs_data_t *res);
