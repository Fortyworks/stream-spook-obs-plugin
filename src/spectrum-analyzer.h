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
 * 音の波形を「帯ごとの強さ」に直す計算。
 *
 * ─── ここに OBS の話を持ち込まない ─────────────────────────────────────────
 * 音を実際に受け取るところ（libobs の audio callback）は spectrum.c に置く。
 * こちらは float のサンプル列を受け取って 0〜255 の配列を返すだけなので、
 * libobs を読み込まずに単体で確かめられる（tools/smoke/analyzer-test.c）。
 *
 * ─── 出てくる値は StreamSpook 本体の解析と同じ形 ─────────────────────────
 * 本体（Rust 側）が PC の音を拾うときも、同じ点数・同じ帯の切り方・同じ底で
 * 0〜255 に直している。ここが違うと、「PC の音」と「OBS のソース」を切り替えた
 * ときに同じ曲でも棒の高さが変わる。
 *
 * ─── なぜ自前の FFT なのか ─────────────────────────────────────────────────
 * 2048 点の変換を毎秒 30 回で、1 回あたり数十マイクロ秒。ライブラリを足すほどの
 * 量ではない。素直な radix-2 を置いて、素朴な DFT と突き合わせるテストで
 * 正しさを見張る。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* FFT の点数。48kHz なら 1 ビン ≒ 23Hz、窓の長さ ≒ 43ms。
 * これより短くすると低音の分解能が足りず、長くすると速い音の立ち上がりが鈍る */
#define SPECTRUM_FFT_SIZE 2048

/* 送る帯の数。オーバーレイの棒の本数とは別で、棒へまとめるのは受け取った側 */
#define SPECTRUM_BANDS 64

/* 帯を切る下端・上端（Hz）。下は暗騒音が乗るだけ、上はエンコードで削れる */
#define SPECTRUM_MIN_HZ 30.0f
#define SPECTRUM_MAX_HZ 16000.0f

/* これより静かなものは 0 として扱う（dBFS） */
#define SPECTRUM_FLOOR_DB (-70.0f)

/* 高い帯を持ち上げる量（dB / オクターブ）。音楽も声も高い音ほど
 * エネルギーが小さいので、そのまま描くと右側が寝たままになる */
#define SPECTRUM_TILT_DB_PER_OCT 2.5f

struct spectrum_band {
	size_t lo, hi; /* ビンの範囲。hi は含まない */
};

struct spectrum_analyzer {
	size_t size;
	float *window;  /* ハン窓 */
	float win_gain; /* 窓の目減りを戻す係数。フルスケールの正弦波が 1.0 になる */
	float *cos_tw;
	float *sin_tw;
	uint32_t *rev;
	float *re;
	float *im;
	struct spectrum_band *bands;
	float *tilt_db; /* 帯ごとの持ち上げ量（dB） */
	size_t band_count;
};

/* 窓と回転因子を作り置きする。size は 2 の冪（4 以上）。失敗（メモリ）なら false */
bool spectrum_analyzer_init(struct spectrum_analyzer *a, size_t size, float sample_rate, size_t band_count,
			    float min_hz, float max_hz, float tilt_db_per_oct);

void spectrum_analyzer_free(struct spectrum_analyzer *a);

/* 直近のサンプル列から帯ごとの強さ（0〜255）を出す。
 * samples は末尾が最新。count が size より短ければ足りないぶんを 0 で埋める。
 * out は band_count 個 */
void spectrum_analyzer_run(struct spectrum_analyzer *a, const float *samples, size_t count, float floor_db,
			   uint8_t *out);

/* re / im に入っているものをその場で複素 FFT にかける（テストが素朴な DFT と比べる用） */
void spectrum_analyzer_fft(struct spectrum_analyzer *a);
