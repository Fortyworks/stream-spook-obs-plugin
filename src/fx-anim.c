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
#include "fx-anim.h"

#include <math.h>
#include <string.h>

static inline float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

/* 3t^2 - 2t^3。両端でなめらかに止まる */
static inline float smooth01(float t)
{
	t = clampf(t, 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

/* xorshift32。乱数の質は要らないが、ちらつきが毎回同じ並びだと
 * 「録画したもの」に見えるので、インスタンスごとに種を変える */
static inline uint32_t rng_next(uint32_t *s)
{
	uint32_t x = *s;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*s = x;
	return x;
}

static inline float rng_signed(uint32_t *s)
{
	return (float)(rng_next(s) % 20001) / 10000.0f - 1.0f;
}

void fx_anim_init(struct fx_anim *a, float lo, float hi)
{
	memset(a, 0, sizeof(*a));
	a->lo = lo;
	a->hi = hi;
	a->period_sec = 1.0f;
	/* 種が 0 だと xorshift が動かない。アドレスの下位ビットは
	 * インスタンスごとに違うので、それをそのまま使う */
	a->rng = (uint32_t)(uintptr_t)a | 1u;
}

void fx_anim_set_target(struct fx_anim *a, float target, float ease_sec)
{
	if (!a->primed) {
		a->primed = true;
		a->from = a->target = a->base = a->out = target;
		a->ease_sec = 0.0f;
		a->ease_t = 0.0f;
		return;
	}
	if (target == a->target) {
		a->ease_sec = ease_sec;
		return;
	}
	a->from = a->base;
	a->target = target;
	a->ease_sec = ease_sec;
	a->ease_t = 0.0f;
}

void fx_anim_set_osc(struct fx_anim *a, int mode, float amount, float period_sec)
{
	if (mode != a->mode) {
		a->phase = 0.0f;
		a->seg_t = 0.0f;
		a->seg_from = 0.0f;
		a->seg_to = rng_signed(&a->rng);
	}
	a->mode = mode;
	a->amount = amount;
	a->period_sec = period_sec > 0.01f ? period_sec : 0.01f;
}

void fx_anim_tick(struct fx_anim *a, float dt)
{
	if (a->ease_sec > 0.0f && a->ease_t < a->ease_sec) {
		a->ease_t += dt;
		a->base = a->from + (a->target - a->from) * smooth01(a->ease_t / a->ease_sec);
	} else {
		a->base = a->target;
	}

	float osc = 0.0f;
	switch (a->mode) {
	case FX_ANIM_PULSE:
		a->phase += dt / a->period_sec;
		a->phase -= floorf(a->phase);
		osc = sinf(a->phase * 6.28318530718f);
		break;
	case FX_ANIM_RANDOM:
		a->seg_t += dt;
		while (a->seg_t >= a->period_sec) {
			a->seg_t -= a->period_sec;
			a->seg_from = a->seg_to;
			a->seg_to = rng_signed(&a->rng);
		}
		osc = a->seg_from + (a->seg_to - a->seg_from) * smooth01(a->seg_t / a->period_sec);
		break;
	default:
		break;
	}

	a->out = clampf(a->base + a->amount * osc, a->lo, a->hi);
}

/* ---- 設定 UI ---- */

void fx_anim_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, FX_ANIM_KEY_MODE, FX_ANIM_NONE);
	obs_data_set_default_double(settings, FX_ANIM_KEY_AMOUNT, 0.3);
	obs_data_set_default_double(settings, FX_ANIM_KEY_PERIOD, 1.0);
	obs_data_set_default_int(settings, FX_ANIM_KEY_TRANSITION, 0);
}

static bool anim_mode_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	const bool on = obs_data_get_int(settings, FX_ANIM_KEY_MODE) != FX_ANIM_NONE;
	obs_property_set_visible(obs_properties_get(props, FX_ANIM_KEY_AMOUNT), on);
	obs_property_set_visible(obs_properties_get(props, FX_ANIM_KEY_PERIOD), on);
	return true;
}

static void add_transition(obs_properties_t *props)
{
	obs_property_t *tr = obs_properties_add_int_slider(props, FX_ANIM_KEY_TRANSITION,
							   obs_module_text("Anim.Transition"), 0, 5000, 10);
	obs_property_int_set_suffix(tr, " ms");
	obs_property_set_long_description(tr, obs_module_text("Anim.Transition.Desc"));
}

void fx_anim_transition_property(obs_properties_t *props)
{
	add_transition(props);
}

void fx_anim_properties(obs_properties_t *props, const char *group_text_key, float amount_max)
{
	obs_properties_t *g = obs_properties_create();

	obs_property_t *mode = obs_properties_add_list(g, FX_ANIM_KEY_MODE, obs_module_text("Anim.Mode"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, obs_module_text("Anim.Mode.None"), FX_ANIM_NONE);
	obs_property_list_add_int(mode, obs_module_text("Anim.Mode.Pulse"), FX_ANIM_PULSE);
	obs_property_list_add_int(mode, obs_module_text("Anim.Mode.Random"), FX_ANIM_RANDOM);
	obs_property_set_modified_callback(mode, anim_mode_modified);

	obs_properties_add_float_slider(g, FX_ANIM_KEY_AMOUNT, obs_module_text("Anim.Amount"), 0.0, (double)amount_max,
					0.01);
	obs_property_t *period = obs_properties_add_float_slider(g, FX_ANIM_KEY_PERIOD, obs_module_text("Anim.Period"),
								 0.05, 10.0, 0.05);
	obs_property_float_set_suffix(period, " s");

	add_transition(g);

	obs_properties_add_group(props, "anim", obs_module_text(group_text_key), OBS_GROUP_NORMAL, g);
}

void fx_anim_read(struct fx_anim *a, obs_data_t *settings)
{
	fx_anim_set_osc(a, (int)obs_data_get_int(settings, FX_ANIM_KEY_MODE),
			(float)obs_data_get_double(settings, FX_ANIM_KEY_AMOUNT),
			(float)obs_data_get_double(settings, FX_ANIM_KEY_PERIOD));
}

float fx_anim_transition_sec(obs_data_t *settings)
{
	return (float)obs_data_get_int(settings, FX_ANIM_KEY_TRANSITION) / 1000.0f;
}
