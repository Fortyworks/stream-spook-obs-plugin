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
 * 帯の計算（src/spectrum-analyzer.c）を libobs 無しで確かめる。
 *
 * 見ているのは StreamSpook 本体（Rust 側の spectrum.rs）のテストと同じ 5 つ:
 *   1. FFT が素朴な DFT と一致する
 *   2. 無音は全部 0
 *   3. フルスケールの 1kHz は自分の帯で上まで行き、立つのは 1〜3 本
 *   4. -40dB の音は底（-70dB）から 30/70 の高さに来る
 *   5. 低い音と高い音は別の帯に、対数の間隔で出る
 *
 * ビルド: cmake --build build_x64 --target spectrum-analyzer-test（Run-Smoke.ps1 が回す）
 * 終了コード 0 で OK。
 */
#define _USE_MATH_DEFINES
#include "spectrum-analyzer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SR 48000.0f

static int failures = 0;

#define CHECK(cond, ...)                                            \
	do {                                                        \
		if (cond) {                                         \
			printf("OK  " __VA_ARGS__);                 \
			printf("\n");                               \
		} else {                                            \
			printf("NG  " __VA_ARGS__);                 \
			printf("\n");                               \
			failures++;                                 \
		}                                                   \
	} while (0)

static void sine(float *out, size_t n, float freq, float amp)
{
	for (size_t i = 0; i < n; i++)
		out[i] = amp * sinf((float)(2.0 * M_PI) * freq * (float)i / SR);
}

static uint8_t max_of(const uint8_t *v, size_t n, size_t *at)
{
	uint8_t best = 0;
	size_t idx = 0;
	for (size_t i = 0; i < n; i++)
		if (v[i] > best) {
			best = v[i];
			idx = i;
		}
	if (at)
		*at = idx;
	return best;
}

/* 1. 素朴な DFT と突き合わせる（窓を 1 にして素の値で比べる） */
static void test_fft_matches_dft(void)
{
	const size_t n = 64;
	struct spectrum_analyzer a;
	spectrum_analyzer_init(&a, n, SR, 8, SPECTRUM_MIN_HZ, SPECTRUM_MAX_HZ, 0.0f);
	float input[64];
	for (size_t i = 0; i < n; i++)
		input[i] = sinf((float)(i * 7) * 0.31f) + 0.4f * cosf((float)(i * 3) * 1.7f);
	memcpy(a.re, input, sizeof(input));
	memset(a.im, 0, n * sizeof(float));
	spectrum_analyzer_fft(&a);

	float worst = 0.0f;
	for (size_t k = 0; k < n; k++) {
		double re = 0.0, im = 0.0;
		for (size_t t = 0; t < n; t++) {
			const double ang = -2.0 * M_PI * (double)(k * t) / (double)n;
			re += input[t] * cos(ang);
			im += input[t] * sin(ang);
		}
		const float dre = fabsf(a.re[k] - (float)re);
		const float dim = fabsf(a.im[k] - (float)im);
		if (dre > worst)
			worst = dre;
		if (dim > worst)
			worst = dim;
	}
	CHECK(worst < 1e-2f, "fft matches naive dft (worst diff %g)", worst);
	spectrum_analyzer_free(&a);
}

/* 2. 無音は全部 0 */
static void test_silence(void)
{
	struct spectrum_analyzer a;
	spectrum_analyzer_init(&a, SPECTRUM_FFT_SIZE, SR, SPECTRUM_BANDS, SPECTRUM_MIN_HZ, SPECTRUM_MAX_HZ,
			       SPECTRUM_TILT_DB_PER_OCT);
	float *zero = calloc(SPECTRUM_FFT_SIZE, sizeof(float));
	uint8_t out[SPECTRUM_BANDS];
	spectrum_analyzer_run(&a, zero, SPECTRUM_FFT_SIZE, SPECTRUM_FLOOR_DB, out);
	CHECK(max_of(out, SPECTRUM_BANDS, NULL) == 0, "silence is all zero");
	free(zero);
	spectrum_analyzer_free(&a);
}

/* 3. フルスケールの正弦波は上まで行く（持ち上げを切って素の高さで見る） */
static void test_full_scale(void)
{
	struct spectrum_analyzer a;
	spectrum_analyzer_init(&a, SPECTRUM_FFT_SIZE, SR, SPECTRUM_BANDS, SPECTRUM_MIN_HZ, SPECTRUM_MAX_HZ, 0.0f);
	float *buf = malloc(SPECTRUM_FFT_SIZE * sizeof(float));
	uint8_t out[SPECTRUM_BANDS];
	sine(buf, SPECTRUM_FFT_SIZE, 1000.0f, 1.0f);
	spectrum_analyzer_run(&a, buf, SPECTRUM_FFT_SIZE, SPECTRUM_FLOOR_DB, out);

	const uint8_t top = max_of(out, SPECTRUM_BANDS, NULL);
	CHECK(top >= 245, "full-scale 1 kHz reaches the top (%u)", (unsigned)top);
	size_t loud = 0;
	for (size_t i = 0; i < SPECTRUM_BANDS; i++)
		if (out[i] > 128)
			loud++;
	CHECK(loud <= 3, "1 kHz lights at most 3 bands (%zu)", loud);
	free(buf);
	spectrum_analyzer_free(&a);
}

/* 4. -40dB は底（-70dB）から 30/70 の高さ */
static void test_quieter_lands_lower(void)
{
	struct spectrum_analyzer a;
	spectrum_analyzer_init(&a, SPECTRUM_FFT_SIZE, SR, SPECTRUM_BANDS, SPECTRUM_MIN_HZ, SPECTRUM_MAX_HZ, 0.0f);
	float *buf = malloc(SPECTRUM_FFT_SIZE * sizeof(float));
	uint8_t loud[SPECTRUM_BANDS], quiet[SPECTRUM_BANDS];
	sine(buf, SPECTRUM_FFT_SIZE, 1000.0f, 1.0f);
	spectrum_analyzer_run(&a, buf, SPECTRUM_FFT_SIZE, SPECTRUM_FLOOR_DB, loud);
	sine(buf, SPECTRUM_FFT_SIZE, 1000.0f, 0.01f);
	spectrum_analyzer_run(&a, buf, SPECTRUM_FFT_SIZE, SPECTRUM_FLOOR_DB, quiet);

	const float hi = (float)max_of(loud, SPECTRUM_BANDS, NULL);
	const float lo = (float)max_of(quiet, SPECTRUM_BANDS, NULL);
	const float want = (30.0f / 70.0f) * 255.0f;
	CHECK(fabsf(lo - want) < 20.0f, "-40 dB lands near %.0f (%.0f)", want, lo);
	CHECK(lo < hi, "quieter is lower (%.0f < %.0f)", lo, hi);
	free(buf);
	spectrum_analyzer_free(&a);
}

/* 5. 低い音と高い音は別の帯、対数の間隔 */
static void test_low_high_bands(void)
{
	struct spectrum_analyzer a;
	spectrum_analyzer_init(&a, SPECTRUM_FFT_SIZE, SR, SPECTRUM_BANDS, SPECTRUM_MIN_HZ, SPECTRUM_MAX_HZ, 0.0f);
	float *buf = malloc(SPECTRUM_FFT_SIZE * sizeof(float));
	uint8_t low[SPECTRUM_BANDS], high[SPECTRUM_BANDS];
	sine(buf, SPECTRUM_FFT_SIZE, 100.0f, 1.0f);
	spectrum_analyzer_run(&a, buf, SPECTRUM_FFT_SIZE, SPECTRUM_FLOOR_DB, low);
	sine(buf, SPECTRUM_FFT_SIZE, 8000.0f, 1.0f);
	spectrum_analyzer_run(&a, buf, SPECTRUM_FFT_SIZE, SPECTRUM_FLOOR_DB, high);

	size_t low_at = 0, high_at = 0;
	max_of(low, SPECTRUM_BANDS, &low_at);
	max_of(high, SPECTRUM_BANDS, &high_at);
	CHECK(low_at < high_at, "100 Hz (%zu) sits left of 8 kHz (%zu)", low_at, high_at);
	CHECK(high_at - low_at > SPECTRUM_BANDS / 3, "bands are log-spaced (%zu apart)", high_at - low_at);

	free(buf);
	spectrum_analyzer_free(&a);
}

/* 帯は昇順で、空の帯は無い。持ち上げは高い帯ほど大きい（本番の形で見る） */
static void test_band_shape(void)
{
	struct spectrum_analyzer a;
	spectrum_analyzer_init(&a, SPECTRUM_FFT_SIZE, SR, SPECTRUM_BANDS, SPECTRUM_MIN_HZ, SPECTRUM_MAX_HZ,
			       SPECTRUM_TILT_DB_PER_OCT);
	int ordered = 1;
	for (size_t i = 0; i < a.band_count; i++) {
		if (a.bands[i].lo >= a.bands[i].hi || a.bands[i].hi > SPECTRUM_FFT_SIZE / 2)
			ordered = 0;
		if (i > 0 && a.bands[i].hi < a.bands[i - 1].hi)
			ordered = 0;
	}
	CHECK(a.band_count == SPECTRUM_BANDS, "band count is %d (%zu)", SPECTRUM_BANDS, a.band_count);
	CHECK(ordered, "bands are ordered and never empty");
	CHECK(a.tilt_db[a.band_count - 1] > a.tilt_db[0], "tilt grows toward high bands");
	spectrum_analyzer_free(&a);
}

/* 帯をビン数より多く求められても壊れない（上端で詰まるだけ） */
static void test_many_bands(void)
{
	struct spectrum_analyzer a;
	CHECK(spectrum_analyzer_init(&a, 256, SR, 200, SPECTRUM_MIN_HZ, SPECTRUM_MAX_HZ, 0.0f),
	      "200 bands on 256 points");
	float buf[256];
	uint8_t *out = malloc(200);
	sine(buf, 256, 1000.0f, 1.0f);
	spectrum_analyzer_run(&a, buf, 256, SPECTRUM_FLOOR_DB, out);
	CHECK(a.band_count == 200, "band count kept (%zu)", a.band_count);
	free(out);
	spectrum_analyzer_free(&a);
}

/* 短い入力は 0 で埋める（鳴り始めの一瞬） */
static void test_short_input(void)
{
	struct spectrum_analyzer a;
	spectrum_analyzer_init(&a, SPECTRUM_FFT_SIZE, SR, SPECTRUM_BANDS, SPECTRUM_MIN_HZ, SPECTRUM_MAX_HZ, 0.0f);
	float buf[512];
	uint8_t out[SPECTRUM_BANDS];
	sine(buf, 512, 1000.0f, 1.0f);
	spectrum_analyzer_run(&a, buf, 512, SPECTRUM_FLOOR_DB, out);
	CHECK(max_of(out, SPECTRUM_BANDS, NULL) > 0, "short input still produces bands");
	spectrum_analyzer_free(&a);
}

int main(void)
{
	test_fft_matches_dft();
	test_silence();
	test_full_scale();
	test_quieter_lands_lower();
	test_low_high_bands();
	test_band_shape();
	test_many_bands();
	test_short_input();
	printf("failures=%d\n", failures);
	return failures ? 1 : 0;
}
