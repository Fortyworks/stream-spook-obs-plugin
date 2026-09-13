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
 * フィルタ 5 つが共通で使う小物。
 *
 * 描き方そのもの（シェーダー）は data/effects/*.effect に置く。
 * ここには「effect を読む」「対象の大きさを取る」のように、
 * どのフィルタでも同じ手順になるものだけを置く。
 */
#pragma once

#include <obs-module.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>

#include "plugin-support.h"

/* data/effects/<file> を読む。グラフィックスのロックは中で取る。
 * 失敗したら NULL を返してログに残す（フィルタは作らず、素通しにする） */
static inline gs_effect_t *fx_load_effect(const char *file)
{
	char *path = obs_module_file(file);
	if (!path) {
		obs_log(LOG_ERROR, "effect not found: %s", file);
		return NULL;
	}

	obs_enter_graphics();
	char *err = NULL;
	gs_effect_t *effect = gs_effect_create_from_file(path, &err);
	obs_leave_graphics();

	if (!effect)
		obs_log(LOG_ERROR, "failed to load %s: %s", file, err ? err : "(no message)");

	bfree(err);
	bfree(path);
	return effect;
}

static inline void fx_destroy_effect(gs_effect_t *effect)
{
	if (!effect)
		return;
	obs_enter_graphics();
	gs_effect_destroy(effect);
	obs_leave_graphics();
}

/* フィルタが掛かる相手（ソース、または手前のフィルタ）のピクセル寸法 */
static inline struct vec2 fx_target_size(obs_source_t *filter)
{
	obs_source_t *target = obs_filter_get_target(filter);
	struct vec2 size;
	vec2_set(&size, (float)obs_source_get_base_width(target), (float)obs_source_get_base_height(target));
	if (size.x < 1.0f)
		size.x = 1.0f;
	if (size.y < 1.0f)
		size.y = 1.0f;
	return size;
}

static inline float fx_getf(obs_data_t *settings, const char *key)
{
	return (float)obs_data_get_double(settings, key);
}

/* 経過時間。float の精度が落ちる前に折り返す。乱数は時間をハッシュして
 * 作っているので、折り返しの瞬間に絵が飛んでも「乱れ」の中に紛れる */
static inline void fx_advance_time(float *time, float seconds, float speed)
{
	*time += seconds * speed;
	if (*time > 1000.0f)
		*time -= 1000.0f;
}
