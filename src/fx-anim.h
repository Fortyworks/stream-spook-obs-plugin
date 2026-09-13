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
 * 値をなめらかに動かすための共通部品。
 *
 * どのフィルタも「強さ」を 1 つ持っていて、それを
 *   1. 設定を変えたときにじわっと移す（transition_ms）
 *   2. 勝手に揺らす（anim_mode: 脈打つ / ちらつく）
 * の 2 つで動かしたい。フィルタごとに書き起こすと、脈の打ち方も
 * 設定キーの名前も面ごとにズレるので、ここに 1 つだけ置く。
 *
 * 設定キーは全フィルタで同じ（anim_mode / anim_amount / anim_period /
 * transition_ms）。アプリ側（obs-websocket の SetSourceFilterSettings）が
 * フィルタの種類を見ずに同じキーで叩けるようにするため。
 */
#pragma once

#include <obs-module.h>
#include <stdbool.h>
#include <stdint.h>

enum fx_anim_mode {
	FX_ANIM_NONE = 0,
	FX_ANIM_PULSE = 1,  /* 正弦波で脈打つ */
	FX_ANIM_RANDOM = 2, /* 乱数で揺れる（ちらつき） */
};

struct fx_anim {
	float lo, hi; /* 出力の範囲 */

	/* 設定値への移り変わり（イージング） */
	bool primed; /* 最初の 1 回だけは移さず、その値に飛ぶ */
	float from, target;
	float ease_sec, ease_t;
	float base; /* 移した結果（揺らす前） */

	/* 揺らし */
	int mode;
	float amount;           /* 振れ幅（値の単位そのまま） */
	float period_sec;       /* 1 周期（脈）/ 1 区間（乱数）の秒数 */
	float phase;            /* 脈: 0..1 */
	float seg_t;            /* 乱数: 今の区間で経った秒数 */
	float seg_from, seg_to; /* 乱数: 区間の両端（-1..1） */
	uint32_t rng;

	float out; /* 今フレームの値 */
};

void fx_anim_init(struct fx_anim *a, float lo, float hi);

/* 設定から来た値を目標にする。目標が変わっていなければ何もしない
 * （関係ない項目を触るたびに移し直しが始まらないように） */
void fx_anim_set_target(struct fx_anim *a, float target, float ease_sec);

void fx_anim_set_osc(struct fx_anim *a, int mode, float amount, float period_sec);

/* 毎フレーム呼ぶ（video_tick） */
void fx_anim_tick(struct fx_anim *a, float dt);

static inline float fx_anim_value(const struct fx_anim *a)
{
	return a->out;
}

/* ---- 設定 UI とキー ---- */

#define FX_ANIM_KEY_MODE "anim_mode"
#define FX_ANIM_KEY_AMOUNT "anim_amount"
#define FX_ANIM_KEY_PERIOD "anim_period"
#define FX_ANIM_KEY_TRANSITION "transition_ms"

void fx_anim_defaults(obs_data_t *settings);

/* 「アニメーション」グループを足す。group_text_key はグループ見出しの
 * 文言キー（何を揺らすかを見出しに書く）、amount_max は振れ幅スライダーの
 * 右端（揺らす値の最大値と同じにする） */
void fx_anim_properties(obs_properties_t *props, const char *group_text_key, float amount_max);

/* 揺らしを持たない面（モザイク）向けに、移り変わりの時間だけを足す */
void fx_anim_transition_property(obs_properties_t *props);

/* 設定から揺らしと移り変わりの時間を読む。目標値はフィルタごとに違うので
 * fx_anim_set_target は呼ぶ側が呼ぶ */
void fx_anim_read(struct fx_anim *a, obs_data_t *settings);
float fx_anim_transition_sec(obs_data_t *settings);
