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
/* 帯ごとの強さの計算。考え方は spectrum-analyzer.h の頭 */
#define _USE_MATH_DEFINES
#include "spectrum-analyzer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const float TAU = (float)(2.0 * M_PI);

static bool is_pow2(size_t n)
{
	return n >= 4 && (n & (n - 1)) == 0;
}

/* 帯ごとのビン範囲と持ち上げ量を作る。
 *
 * 対数で切るのは、人の耳が周波数を対数で聞いているため。等間隔にすると、
 * 左端の 1/4 に音楽のほとんどが入って、残りがずっと平らな絵になる */
static void build_bands(struct spectrum_analyzer *a, float sample_rate, float min_hz, float max_hz,
			float tilt_db_per_oct)
{
	const float nyquist = sample_rate / 2.0f;
	const float bin_hz = sample_rate / (float)a->size;
	const size_t max_bin = a->size / 2;
	const size_t n = a->band_count;

	const float lo_hz = fmaxf(min_hz, bin_hz);
	float hi_hz = fminf(max_hz, nyquist * 0.98f);
	if (hi_hz < lo_hz * 2.0f)
		hi_hz = lo_hz * 2.0f;
	const float ratio = hi_hz / lo_hz;

	size_t prev_end = (size_t)fmaxf(floorf(lo_hz / bin_hz), 1.0f);

	for (size_t i = 0; i < n; i++) {
		const float f_hi = lo_hz * powf(ratio, (float)(i + 1) / (float)n);
		size_t end = (size_t)roundf(f_hi / bin_hz);
		/* 帯が 1 ビンも持たないと、低音側で同じビンを何本もの帯が指して
		 * 「左端が全部同じ高さ」になる。必ず 1 つは進める */
		if (end <= prev_end)
			end = prev_end + 1;
		if (end > max_bin)
			end = max_bin;
		size_t start = prev_end;
		if (start > end - 1)
			start = end - 1;
		a->bands[i].lo = start;
		a->bands[i].hi = end;

		const float f_lo = lo_hz * powf(ratio, (float)i / (float)n);
		const float center = sqrtf(f_lo * f_hi);
		a->tilt_db[i] = tilt_db_per_oct * log2f(center / lo_hz);

		prev_end = end;
		if (prev_end >= max_bin) {
			/* 上端まで来たら、残りの帯は最後のビンを指したままにする
			 * （バンド数を増やしすぎたとき用の逃げ） */
			for (size_t j = i + 1; j < n; j++) {
				a->bands[j].lo = max_bin - 1;
				a->bands[j].hi = max_bin;
				const float f = lo_hz * powf(ratio, ((float)j + 0.5f) / (float)n);
				a->tilt_db[j] = tilt_db_per_oct * log2f(f / lo_hz);
			}
			break;
		}
	}
}

bool spectrum_analyzer_init(struct spectrum_analyzer *a, size_t size, float sample_rate, size_t band_count,
			    float min_hz, float max_hz, float tilt_db_per_oct)
{
	memset(a, 0, sizeof(*a));
	if (!is_pow2(size))
		return false;
	if (band_count < 1)
		band_count = 1;

	a->size = size;
	a->band_count = band_count;
	a->window = calloc(size, sizeof(float));
	a->cos_tw = calloc(size, sizeof(float));
	a->sin_tw = calloc(size, sizeof(float));
	a->rev = calloc(size, sizeof(uint32_t));
	a->re = calloc(size, sizeof(float));
	a->im = calloc(size, sizeof(float));
	a->bands = calloc(band_count, sizeof(struct spectrum_band));
	a->tilt_db = calloc(band_count, sizeof(float));
	if (!a->window || !a->cos_tw || !a->sin_tw || !a->rev || !a->re || !a->im || !a->bands || !a->tilt_db) {
		spectrum_analyzer_free(a);
		return false;
	}

	/* ハン窓。切り出した端の不連続を消す（そのまま切ると、切れ目が
	 * 全帯域に広がった雑音として乗る） */
	float win_sum = 0.0f;
	for (size_t i = 0; i < size; i++) {
		const float t = (float)i / (float)size;
		a->window[i] = 0.5f - 0.5f * cosf(TAU * t);
		win_sum += a->window[i];
	}
	/* 実数の正弦波は正負 2 本のビンに割れるので、片側だけを見るぶん 2 倍する */
	a->win_gain = 2.0f / fmaxf(win_sum, 1e-30f);

	for (size_t t = 0; t < size; t++) {
		const float ang = -TAU * (float)t / (float)size;
		a->cos_tw[t] = cosf(ang);
		a->sin_tw[t] = sinf(ang);
	}

	unsigned bits = 0;
	while (((size_t)1 << bits) < size)
		bits++;
	for (size_t i = 0; i < size; i++) {
		uint32_t r = 0;
		for (unsigned b = 0; b < bits; b++)
			if (i & ((size_t)1 << b))
				r |= 1u << (bits - 1 - b);
		a->rev[i] = r;
	}

	build_bands(a, sample_rate, min_hz, max_hz, tilt_db_per_oct);
	return true;
}

void spectrum_analyzer_free(struct spectrum_analyzer *a)
{
	free(a->window);
	free(a->cos_tw);
	free(a->sin_tw);
	free(a->rev);
	free(a->re);
	free(a->im);
	free(a->bands);
	free(a->tilt_db);
	memset(a, 0, sizeof(*a));
}

/* その場で複素 FFT をかける（radix-2、時間間引き） */
void spectrum_analyzer_fft(struct spectrum_analyzer *a)
{
	const size_t n = a->size;
	float *re = a->re;
	float *im = a->im;

	for (size_t i = 0; i < n; i++) {
		const size_t j = a->rev[i];
		if (j > i) {
			float t = re[i];
			re[i] = re[j];
			re[j] = t;
			t = im[i];
			im[i] = im[j];
			im[j] = t;
		}
	}

	for (size_t len = 2; len <= n; len <<= 1) {
		const size_t half = len / 2;
		const size_t step = n / len;
		for (size_t start = 0; start < n; start += len) {
			for (size_t k = 0; k < half; k++) {
				const size_t tw = k * step;
				const float c = a->cos_tw[tw];
				const float s = a->sin_tw[tw];
				const size_t i0 = start + k;
				const size_t i1 = i0 + half;
				const float tr = re[i1] * c - im[i1] * s;
				const float ti = re[i1] * s + im[i1] * c;
				re[i1] = re[i0] - tr;
				im[i1] = im[i0] - ti;
				re[i0] += tr;
				im[i0] += ti;
			}
		}
	}
}

void spectrum_analyzer_run(struct spectrum_analyzer *a, const float *samples, size_t count, float floor_db,
			   uint8_t *out)
{
	const size_t n = a->size;
	const size_t take = count < n ? count : n;
	const size_t pad = n - take;
	const float *src = samples + (count - take);

	for (size_t i = 0; i < pad; i++) {
		a->re[i] = 0.0f;
		a->im[i] = 0.0f;
	}
	for (size_t i = 0; i < take; i++) {
		a->re[pad + i] = src[i] * a->window[pad + i];
		a->im[pad + i] = 0.0f;
	}

	spectrum_analyzer_fft(a);

	const float floor_v = fminf(floor_db, -1.0f);
	const float span = -floor_v;

	for (size_t bi = 0; bi < a->band_count; bi++) {
		/* 帯の中はいちばん強いビンを採る。平均だと、幅の広い高音側で
		 * 単独の音（シンバルの芯）が周りに薄められて出てこなくなる */
		float peak = 0.0f;
		for (size_t k = a->bands[bi].lo; k < a->bands[bi].hi; k++) {
			const float m = a->re[k] * a->re[k] + a->im[k] * a->im[k];
			if (m > peak)
				peak = m;
		}
		const float amp = sqrtf(peak) * a->win_gain;
		/* 持ち上げ（tilt）は測った値にだけ効かせる。底に足してしまうと、
		 * 無音でも右へ行くほど高くなる階段が出る */
		float t;
		if (amp <= 1e-9f) {
			t = 0.0f;
		} else {
			const float db = 20.0f * log10f(amp) + a->tilt_db[bi];
			t = (db - floor_v) / span;
			if (t < 0.0f)
				t = 0.0f;
			if (t > 1.0f)
				t = 1.0f;
		}
		out[bi] = (uint8_t)(t * 255.0f + 0.5f);
	}
}
