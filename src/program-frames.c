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
 * 配信に出て行く絵（プログラム出力）を小さな JPEG にして、obs-websocket 経由で
 * アプリへ渡す。
 *
 * ─── 何に使うか ────────────────────────────────────────────────────────────
 * アプリの「お絵かきレイヤー」は、配信者が描いた絵を配信画面へ重ねる。描く
 * キャンバスの下にいまの配信画面を敷きたいので、その絵をここから取る。
 * obs-websocket の GetSourceScreenshot でも取れるが、毎回 PNG / JPEG に組んだ
 * 大きな絵をその場で読み戻す（描画のスレッドが GPU を待つ）ので、何枚も続けて
 * 取るには重い。ここでは描き終わった絵を小さく縮めて、読み戻しも符号化も
 * 描画のスレッドの外でやる。アプリはこの要求が無い版では GetSourceScreenshot に
 * 戻るので、ここは足すだけで何も壊さない。
 *
 * ─── 話し方（obs-websocket の vendor API。vendor 名 "stream-spook"） ────────
 *   要求 frame_subscribe    { canvas?, width }
 *                           → { active, reason?, width, height }
 *                           取り始める（呼ぶたびに期限 LEASE_MS が延びる）。アプリは
 *                           見ているあいだ 2 秒おきに呼び続ける。幅を変えて呼ぶと
 *                           大きさを変える
 *   要求 frame_unsubscribe  { canvas? } → {}
 *   要求 frame_get          { canvas?, since? }
 *                           → { active, seq, width, height, jpeg? }
 *                           いちばん新しい 1 枚。seq が since より新しいときだけ
 *                           jpeg（base64。data: は付けない）を載せる
 *
 * canvas は OBS 31 からのキャンバス（縦の 9:16 など）の uuid。省くか空なら
 * メインのキャンバス。同時に取れるのは MAX_SUBS 枚まで（キャンバスごとに 1 つ）。
 * reason は busy（いっぱい）/ no_canvas（その uuid が無い・映像を持たない）。
 *
 * ─── 描画のスレッドでやること ──────────────────────────────────────────────
 * obs_add_main_rendered_callback（ミックスを 1 つ描き終えるたびに呼ばれる）で、
 * 描き終わった絵をもう一度描かずに、そのテクスチャを小さな texrender へ写すだけ。
 *   メイン        obs_render_main_texture（プロジェクターやスクリーンショットと同じ）
 *   ほかのキャンバス obs_render_canvas_texture（そのキャンバスの描き終わった絵）
 * 写したものは gs_stage_texture で読み戻しの箱へ送り、**次の回**に map する
 * （箱は 2 つを交互に使う）。その場で map すると GPU が写し終えるまで描画の
 * スレッドが止まる。map したら画素を詰めて写し、自前のスレッドへ渡す。
 * 1 秒に MAX_FPS 回まで。誰も取っていないあいだは、呼ばれてもすぐ戻る。
 *
 * ─── 自前のスレッドでやること ──────────────────────────────────────────────
 * JPEG にして（stb_image_write、品質 JPEG_QUALITY）、base64 にして、いちばん
 * 新しい 1 枚として置く。間に合わなければ古い画素は捨てる（溜めない）。
 * 期限が切れた取り口もここで畳む（GPU のものは obs_enter_graphics の中で返す）。
 *
 * ─── 錠の順番 ──────────────────────────────────────────────────────────────
 * 描画のスレッドは「ミックスの一覧の錠 → グラフィックスの文脈 → 描画コールバックの
 * 錠」を持ったまま呼んでくるので、そこで S.mutex を取る。逆向きに待つと詰まるので、
 * **S.mutex を持ったまま obs_enter_graphics・キャンバスを返す・キャンバスを引くを
 * しない。** 取り口を外すときは、一覧から抜くところだけを S.mutex の下でやり、
 * GPU のものとキャンバスは錠の外で返す。描画のスレッドではキャンバスの参照を
 * 取りも返しもしない（最後の参照を返すとキャンバスが壊れ、壊れるときにミックスの
 * 一覧の錠を取りに行く。描画のスレッドはそれを既に持っている）。
 *
 * ─── Streamlabs Desktop では登録しない ──────────────────────────────────────
 * フォークは obs_video_info などの並びが違い、確かめる手段も無いので出さない
 * （host_info の features.frames が false になる。アプリは GetSourceScreenshot に
 * 戻るが、Streamlabs にはそれも無いので、下に敷く絵が出ないだけ）。
 */
#include "program-frames.h"

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "plugin-support.h"
#include "third_party/stb_image_write.h"
#include "vendor.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define REQ_SUBSCRIBE "frame_subscribe"
#define REQ_UNSUBSCRIBE "frame_unsubscribe"
#define REQ_GET "frame_get"

/* subscribe から次の subscribe が来ないと畳むまでの時間 */
#define LEASE_MS 6000
/* 1 秒に何枚まで取るか */
#define MAX_FPS 30
/* 描画の間隔の揺れを許す幅。OBS が 30fps で回っているとき、1 フレームが予定より
 * わずかに早く来ただけで見送って、1 枚おきに落ちないように */
#define FRAME_SLACK_MS 3
/* 同時に取れるキャンバスの数（メイン＋縦で足りる） */
#define MAX_SUBS 2
/* 幅の範囲。高さはキャンバスの縦横比から決める */
#define MIN_WIDTH 160
#define MAX_WIDTH 1280
#define JPEG_QUALITY 70
/* 自前のスレッドが、画素が来なくても期限を見に起きる間隔 */
#define SWEEP_MS 500
#define CANVAS_KEY_MAX 64
#define NUM_STAGES 2

#define MS_TO_NS(ms) ((uint64_t)(ms) * 1000000ULL)

/* ─── キャンバスの関数（OBS 31.1 から） ─────────────────────────────────────
 * 直に呼ぶと、キャンバスを知らない版の OBS ではこの DLL ごと読み込めなくなる
 * （フィルタも音の取り口も道連れになる）。obs.dll から名前で引き、無ければ
 * メインのキャンバスだけを受ける */
static struct {
	obs_canvas_t *(*get_by_uuid)(const char *uuid);
	void (*release)(obs_canvas_t *canvas);
	bool (*get_video_info)(const obs_canvas_t *canvas, struct obs_video_info *ovi);
	bool (*removed)(obs_canvas_t *canvas);
	void (*render_texture)(obs_canvas_t *canvas);
	bool ready;
} C;

static void canvas_api_load(void)
{
	memset(&C, 0, sizeof(C));
#ifdef _WIN32
	HMODULE obs_dll = GetModuleHandleW(L"obs.dll");
	if (!obs_dll)
		return;
	C.get_by_uuid = (obs_canvas_t * (*)(const char *)) GetProcAddress(obs_dll, "obs_get_canvas_by_uuid");
	C.release = (void (*)(obs_canvas_t *))GetProcAddress(obs_dll, "obs_canvas_release");
	C.get_video_info = (bool (*)(const obs_canvas_t *, struct obs_video_info *))GetProcAddress(
		obs_dll, "obs_canvas_get_video_info");
	C.removed = (bool (*)(obs_canvas_t *))GetProcAddress(obs_dll, "obs_canvas_removed");
	C.render_texture = (void (*)(obs_canvas_t *))GetProcAddress(obs_dll, "obs_render_canvas_texture");
#else
	C.get_by_uuid = obs_get_canvas_by_uuid;
	C.release = obs_canvas_release;
	C.get_video_info = obs_canvas_get_video_info;
	C.removed = obs_canvas_removed;
	C.render_texture = obs_render_canvas_texture;
#endif
	C.ready = C.get_by_uuid && C.release && C.get_video_info && C.removed && C.render_texture;
}

/* ─── 取り口 ──────────────────────────────────────────────────────────────── */

struct sub {
	char key[CANVAS_KEY_MAX]; /* キャンバスの uuid。メインは "" */
	uint64_t id;              /* 自前のスレッドが、符号化しているあいだに外されたかを見る */
	obs_canvas_t *canvas;     /* 強い参照。メインは NULL。返すのは S.mutex の外で */

	uint32_t req_width;     /* 頼まれた幅（丸めたあと） */
	uint32_t width, height; /* いま取っている大きさ */
	uint64_t lease_until_ns;
	bool lost; /* キャンバスが消された。次の見回りで畳む */

	/* 描画のスレッドだけが触る */
	gs_texrender_t *texrender;
	gs_stagesurf_t *stage[NUM_STAGES];
	uint32_t stage_w[NUM_STAGES], stage_h[NUM_STAGES];
	int pending;   /* 読み戻しを待っている箱。無ければ -1 */
	int last_slot; /* 最後に送った箱 */
	uint64_t next_capture_ns; /* 次に取る予定の時刻 */

	/* 自前のスレッドへ渡す画素（RGBA を詰めたもの）。S.mutex の下で */
	uint8_t *job;
	uint32_t job_w, job_h;

	/* いちばん新しい 1 枚。S.mutex の下で */
	char *jpeg_b64;
	uint32_t frame_w, frame_h;
	long long seq;
};

static struct {
	bool enabled;

	pthread_mutex_t mutex;
	struct sub *subs[MAX_SUBS];
	size_t count;
	volatile long active; /* count の写し。描画のスレッドが錠を取らずに見る */
	uint64_t next_id;

	bool callback_added;
	pthread_t thread;
	bool thread_started;
	volatile bool stopping;
	os_event_t *wake;
} S;

static void set_active_count(void)
{
	os_atomic_set_long(&S.active, (long)S.count);
}

/* 幅を丸め、キャンバスの縦横比から高さを出す。どちらも偶数 */
static void target_size(uint32_t req_width, uint32_t base_w, uint32_t base_h, uint32_t *w, uint32_t *h)
{
	uint32_t cw = req_width;
	if (cw < MIN_WIDTH)
		cw = MIN_WIDTH;
	if (cw > MAX_WIDTH)
		cw = MAX_WIDTH;
	cw &= ~1u;
	uint32_t ch = 2;
	if (base_w > 0 && base_h > 0) {
		const double half = (double)cw * (double)base_h / (double)base_w / 2.0;
		ch = (uint32_t)(half + 0.5) * 2;
		if (ch < 2)
			ch = 2;
	}
	*w = cw;
	*h = ch;
}

/* キャンバスの基準の大きさ（canvas が NULL ならメイン）。描画のスレッドからは呼ばない */
static bool base_size(obs_canvas_t *canvas, uint32_t *bw, uint32_t *bh)
{
	struct obs_video_info ovi = {0};
	bool ok = canvas ? C.get_video_info(canvas, &ovi) : obs_get_video_info(&ovi);
	if (!ok || ovi.base_width == 0 || ovi.base_height == 0)
		return false;
	*bw = ovi.base_width;
	*bh = ovi.base_height;
	return true;
}

static struct sub *sub_find(const char *key)
{
	for (size_t i = 0; i < S.count; i++)
		if (strcmp(S.subs[i]->key, key) == 0)
			return S.subs[i];
	return NULL;
}

/* 一覧から抜く（S.mutex の下で）。返すのは sub_free で、錠の外から */
static struct sub *sub_take_at(size_t i)
{
	struct sub *s = S.subs[i];
	for (size_t j = i + 1; j < S.count; j++)
		S.subs[j - 1] = S.subs[j];
	S.count--;
	set_active_count();
	return s;
}

/* 一覧から抜いたものを返す。S.mutex を持たずに、描画のスレッドの外から呼ぶ */
static void sub_free(struct sub *s)
{
	if (!s)
		return;
	if (s->texrender || s->stage[0] || s->stage[1]) {
		obs_enter_graphics();
		gs_texrender_destroy(s->texrender);
		for (int i = 0; i < NUM_STAGES; i++)
			gs_stagesurface_destroy(s->stage[i]);
		obs_leave_graphics();
	}
	if (s->canvas)
		C.release(s->canvas);
	bfree(s->job);
	bfree(s->jpeg_b64);
	bfree(s);
}

/* ─── 描画のスレッド ──────────────────────────────────────────────────────── */

/* 前の回に送った箱を読み、画素を詰めて自前のスレッドへ渡す */
static bool collect_pending(struct sub *s)
{
	const int i = s->pending;
	if (i < 0)
		return false;
	s->pending = -1;

	uint8_t *data = NULL;
	uint32_t linesize = 0;
	if (!gs_stagesurface_map(s->stage[i], &data, &linesize))
		return false;

	const uint32_t w = s->stage_w[i], h = s->stage_h[i];
	const size_t row = (size_t)w * 4;
	/* 前の画素がまだ渡っていなければ上書きする（古い絵は要らない） */
	if (s->job && (s->job_w != w || s->job_h != h)) {
		bfree(s->job);
		s->job = NULL;
	}
	if (!s->job)
		s->job = bmalloc(row * h);
	for (uint32_t y = 0; y < h; y++)
		memcpy(s->job + row * y, data + (size_t)linesize * y, row);
	s->job_w = w;
	s->job_h = h;
	gs_stagesurface_unmap(s->stage[i]);
	return true;
}

/* 描き終わった絵を小さく写して、読み戻しの箱へ送る */
static void capture(struct sub *s)
{
	if (s->canvas && C.removed(s->canvas)) {
		s->lost = true;
		return;
	}

	/* 基準の大きさは描くたびに読む（設定で変わっても縦横比が追いつくように）。
	 * どちらも ovi を写すだけで錠を取らないので、ここで呼んでも詰まらない */
	struct obs_video_info ovi = {0};
	const bool ok = s->canvas ? C.get_video_info(s->canvas, &ovi) : obs_get_video_info(&ovi);
	if (!ok || ovi.base_width == 0 || ovi.base_height == 0)
		return;
	uint32_t w, h;
	target_size(s->req_width, ovi.base_width, ovi.base_height, &w, &h);
	s->width = w;
	s->height = h;

	if (!s->texrender)
		s->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (!s->texrender)
		return;

	gs_texrender_reset(s->texrender);
	if (!gs_texrender_begin_with_color_space(s->texrender, w, h, GS_CS_SRGB))
		return;

	struct vec4 zero;
	vec4_zero(&zero);
	gs_clear(GS_CLEAR_COLOR, &zero, 0.0f, 0);
	/* 基準の大きさの座標で描かせて、ビューポート（w x h）へ縮める。
	 * HDR のキャンバスは、描く先が sRGB なので向こうがトーンマップしてくれる */
	gs_ortho(0.0f, (float)ovi.base_width, 0.0f, (float)ovi.base_height, -100.0f, 100.0f);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	if (s->canvas)
		C.render_texture(s->canvas);
	else
		obs_render_main_texture();
	gs_blend_state_pop();
	gs_texrender_end(s->texrender);

	/* さっき map した箱とは別の箱へ送る（GPU が書いている箱を読みに行かないように） */
	const int slot = (s->last_slot + 1) % NUM_STAGES;
	if (s->stage[slot] && (s->stage_w[slot] != w || s->stage_h[slot] != h)) {
		gs_stagesurface_destroy(s->stage[slot]);
		s->stage[slot] = NULL;
	}
	if (!s->stage[slot]) {
		s->stage[slot] = gs_stagesurface_create(w, h, GS_RGBA);
		s->stage_w[slot] = w;
		s->stage_h[slot] = h;
	}
	if (!s->stage[slot])
		return;
	gs_stage_texture(s->stage[slot], gs_texrender_get_texture(s->texrender));
	s->pending = slot;
	s->last_slot = slot;
}

static void on_rendered(void *param)
{
	UNUSED_PARAMETER(param);
	/* 誰も取っていないあいだは錠も取らない（毎フレーム、ミックスの数だけ呼ばれる） */
	if (os_atomic_load_long(&S.active) == 0)
		return;

	const uint64_t now = os_gettime_ns();
	const uint64_t period = 1000000000ULL / MAX_FPS;
	bool handed = false;

	pthread_mutex_lock(&S.mutex);
	for (size_t i = 0; i < S.count; i++) {
		struct sub *s = S.subs[i];
		if (s->lost || now > s->lease_until_ns || s->width == 0 || s->height == 0)
			continue;
		/* 取る時刻は前に取った時刻からではなく、予定から数える。描画の間隔が
		 * 揺れても（遅れた次が早く来ても）、平均で MAX_FPS 枚に揃う */
		if (now + MS_TO_NS(FRAME_SLACK_MS) < s->next_capture_ns)
			continue;
		s->next_capture_ns += period;
		if (s->next_capture_ns + period < now)
			s->next_capture_ns = now + period; /* 大きく遅れたら追いかけずに今から数え直す */
		if (collect_pending(s))
			handed = true;
		capture(s);
	}
	pthread_mutex_unlock(&S.mutex);

	if (handed)
		os_event_signal(S.wake);
}

/* ─── 自前のスレッド（JPEG にして置く・期限の見回り） ─────────────────────── */

struct out_buf {
	uint8_t *data;
	size_t len;
	size_t cap;
};

static void out_write(void *ctx, void *data, int size)
{
	struct out_buf *o = ctx;
	if (size <= 0)
		return;
	if (o->len + (size_t)size > o->cap) {
		size_t cap = o->cap ? o->cap : 64 * 1024;
		while (cap < o->len + (size_t)size)
			cap *= 2;
		o->data = brealloc(o->data, cap);
		o->cap = cap;
	}
	memcpy(o->data + o->len, data, (size_t)size);
	o->len += (size_t)size;
}

/* libobs の util には base64 が無いので、ここで組む */
static char *base64_encode(const uint8_t *in, size_t len)
{
	static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	char *out = bmalloc((len + 2) / 3 * 4 + 1);
	char *p = out;
	size_t i = 0;
	for (; i + 2 < len; i += 3) {
		const uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
		*p++ = T[(v >> 18) & 63];
		*p++ = T[(v >> 12) & 63];
		*p++ = T[(v >> 6) & 63];
		*p++ = T[v & 63];
	}
	if (i < len) {
		uint32_t v = (uint32_t)in[i] << 16;
		if (i + 1 < len)
			v |= (uint32_t)in[i + 1] << 8;
		*p++ = T[(v >> 18) & 63];
		*p++ = T[(v >> 12) & 63];
		*p++ = i + 1 < len ? T[(v >> 6) & 63] : '=';
		*p++ = '=';
	}
	*p = '\0';
	return out;
}

/* 渡された画素を 1 枚ずつ JPEG にする。符号化は錠の外で */
static void encode_jobs(void)
{
	while (!os_atomic_load_bool(&S.stopping)) {
		uint8_t *px = NULL;
		uint32_t w = 0, h = 0;
		uint64_t id = 0;

		pthread_mutex_lock(&S.mutex);
		for (size_t i = 0; i < S.count; i++) {
			struct sub *s = S.subs[i];
			if (s->job) {
				px = s->job;
				w = s->job_w;
				h = s->job_h;
				id = s->id;
				s->job = NULL;
				break;
			}
		}
		pthread_mutex_unlock(&S.mutex);
		if (!px)
			return;

		/* 4 成分のまま渡すと、stb は A を読まずに RGB だけを使う */
		struct out_buf jpg = {0};
		const bool ok = stbi_write_jpg_to_func(out_write, &jpg, (int)w, (int)h, 4, px, JPEG_QUALITY) != 0;
		bfree(px);
		char *b64 = ok && jpg.len > 0 ? base64_encode(jpg.data, jpg.len) : NULL;
		bfree(jpg.data);
		if (!b64)
			continue;

		pthread_mutex_lock(&S.mutex);
		struct sub *s = NULL;
		for (size_t i = 0; i < S.count; i++)
			if (S.subs[i]->id == id)
				s = S.subs[i];
		if (s) {
			bfree(s->jpeg_b64);
			s->jpeg_b64 = b64;
			s->frame_w = w;
			s->frame_h = h;
			s->seq++;
			b64 = NULL;
		}
		pthread_mutex_unlock(&S.mutex);
		bfree(b64); /* 符号化しているあいだに外された */
	}
}

/* 期限が切れた・キャンバスが消された取り口を畳む */
static void sweep(void)
{
	const uint64_t now = os_gettime_ns();
	struct sub *dead[MAX_SUBS];
	size_t n = 0;

	pthread_mutex_lock(&S.mutex);
	for (size_t i = 0; i < S.count;) {
		struct sub *s = S.subs[i];
		if (s->lost || now > s->lease_until_ns) {
			dead[n++] = sub_take_at(i);
			continue;
		}
		i++;
	}
	pthread_mutex_unlock(&S.mutex);

	for (size_t i = 0; i < n; i++)
		sub_free(dead[i]);
}

static void *thread_main(void *unused)
{
	UNUSED_PARAMETER(unused);
	os_set_thread_name("stream-spook-frames");
	while (!os_atomic_load_bool(&S.stopping)) {
		os_event_timedwait(S.wake, SWEEP_MS);
		if (os_atomic_load_bool(&S.stopping))
			break;
		encode_jobs();
		sweep();
	}
	return NULL;
}

/* ─── vendor の要求 ───────────────────────────────────────────────────────── */

/* canvas を取り出す。省いたとき・空はメイン（""）。長すぎるものは NULL */
static const char *req_canvas(obs_data_t *req)
{
	const char *key = req ? obs_data_get_string(req, "canvas") : NULL;
	if (!key)
		return "";
	if (strlen(key) >= CANVAS_KEY_MAX)
		return NULL;
	return key;
}

static void reply_inactive(obs_data_t *res, const char *reason)
{
	obs_data_set_bool(res, "active", false);
	obs_data_set_string(res, "reason", reason);
	obs_data_set_int(res, "width", 0);
	obs_data_set_int(res, "height", 0);
}

static void req_subscribe(obs_data_t *req, obs_data_t *res, void *priv)
{
	UNUSED_PARAMETER(priv);
	const char *key = req_canvas(req);
	if (!key) {
		reply_inactive(res, "no_canvas");
		return;
	}
	long long req_w = req ? obs_data_get_int(req, "width") : 0;
	if (req_w < MIN_WIDTH)
		req_w = MIN_WIDTH;
	if (req_w > MAX_WIDTH)
		req_w = MAX_WIDTH;

	/* キャンバスを引くのは錠の外で（「錠の順番」） */
	obs_canvas_t *canvas = NULL;
	uint32_t bw = 0, bh = 0;
	if (*key) {
		canvas = C.ready ? C.get_by_uuid(key) : NULL;
		if (!canvas || C.removed(canvas) || !base_size(canvas, &bw, &bh)) {
			if (canvas)
				C.release(canvas);
			reply_inactive(res, "no_canvas");
			return;
		}
	} else if (!base_size(NULL, &bw, &bh)) {
		reply_inactive(res, "no_canvas");
		return;
	}
	uint32_t w, h;
	target_size((uint32_t)req_w, bw, bh, &w, &h);

	const uint64_t now = os_gettime_ns();
	struct sub *dead[MAX_SUBS];
	size_t n_dead = 0;
	bool busy = false;

	pthread_mutex_lock(&S.mutex);
	/* 見回りを待たずに、畳むべきものは先に抜いて枠を空ける。キャンバスが消された・
	 * 同じ uuid で別のキャンバスに替わっていた（消して作り直された）ものも作り直す */
	for (size_t i = 0; i < S.count;) {
		struct sub *t = S.subs[i];
		const bool replaced = canvas && strcmp(t->key, key) == 0 && t->canvas != canvas;
		if (t->lost || now > t->lease_until_ns || replaced) {
			dead[n_dead++] = sub_take_at(i);
			continue;
		}
		i++;
	}
	struct sub *s = sub_find(key);
	if (s) {
		s->lease_until_ns = now + MS_TO_NS(LEASE_MS);
		s->req_width = (uint32_t)req_w;
		s->width = w;
		s->height = h;
	} else if (S.count >= MAX_SUBS) {
		busy = true;
	} else {
		s = bzalloc(sizeof(*s));
		strncpy(s->key, key, CANVAS_KEY_MAX - 1);
		s->id = ++S.next_id;
		s->canvas = canvas; /* 参照は持ったまま。sub_free で返す */
		canvas = NULL;
		s->req_width = (uint32_t)req_w;
		s->width = w;
		s->height = h;
		s->lease_until_ns = now + MS_TO_NS(LEASE_MS);
		s->pending = -1;
		s->last_slot = -1;
		S.subs[S.count++] = s;
		set_active_count();
	}
	pthread_mutex_unlock(&S.mutex);

	/* 返すのも錠の外で。既に持っているぶん（呼び直し）はここで 1 つ返す */
	if (canvas)
		C.release(canvas);
	for (size_t i = 0; i < n_dead; i++)
		sub_free(dead[i]);

	if (busy) {
		reply_inactive(res, "busy");
		return;
	}
	obs_data_set_bool(res, "active", true);
	obs_data_set_int(res, "width", w);
	obs_data_set_int(res, "height", h);
}

static void req_unsubscribe(obs_data_t *req, obs_data_t *res, void *priv)
{
	UNUSED_PARAMETER(res);
	UNUSED_PARAMETER(priv);
	const char *key = req_canvas(req);
	if (!key)
		return;
	struct sub *old = NULL;
	pthread_mutex_lock(&S.mutex);
	for (size_t i = 0; i < S.count; i++) {
		if (strcmp(S.subs[i]->key, key) == 0) {
			old = sub_take_at(i);
			break;
		}
	}
	pthread_mutex_unlock(&S.mutex);
	sub_free(old);
}

/* いちばん新しい 1 枚を写して返すだけ。待たない */
static void req_get(obs_data_t *req, obs_data_t *res, void *priv)
{
	UNUSED_PARAMETER(priv);
	const char *key = req_canvas(req);
	const long long since = req ? obs_data_get_int(req, "since") : 0;
	const uint64_t now = os_gettime_ns();
	bool active = false;
	long long seq = 0;
	uint32_t w = 0, h = 0;
	char *jpeg = NULL;

	pthread_mutex_lock(&S.mutex);
	struct sub *s = key ? sub_find(key) : NULL;
	if (s && !s->lost && now <= s->lease_until_ns) {
		active = true;
		seq = s->seq;
		w = seq > 0 ? s->frame_w : s->width;
		h = seq > 0 ? s->frame_h : s->height;
		if (seq > since && s->jpeg_b64)
			jpeg = bstrdup(s->jpeg_b64);
	}
	pthread_mutex_unlock(&S.mutex);

	obs_data_set_bool(res, "active", active);
	obs_data_set_int(res, "seq", seq);
	obs_data_set_int(res, "width", w);
	obs_data_set_int(res, "height", h);
	if (jpeg) {
		obs_data_set_string(res, "jpeg", jpeg);
		bfree(jpeg);
	}
}

/* ─── 立ち上げ・片付け ────────────────────────────────────────────────────── */

void program_frames_init(void)
{
	memset(&S, 0, sizeof(S));
	pthread_mutex_init(&S.mutex, NULL);

	if (!ss_vendor_available()) {
		obs_log(LOG_INFO, "program frames are disabled (no way to talk to the app)");
		return;
	}
	if (ss_host() == SS_HOST_STREAMLABS) {
		obs_log(LOG_INFO, "program frames are disabled on Streamlabs Desktop");
		return;
	}
	canvas_api_load();

	if (os_event_init(&S.wake, OS_EVENT_TYPE_AUTO) != 0) {
		obs_log(LOG_WARNING, "program frames: failed to create wake event");
		return;
	}
	if (pthread_create(&S.thread, NULL, thread_main, NULL) != 0) {
		obs_log(LOG_WARNING, "program frames: failed to start thread");
		os_event_destroy(S.wake);
		S.wake = NULL;
		return;
	}
	S.thread_started = true;

	/* 登録は 1 回だけで、外すのは片付けのとき。取り口の出し入れのたびに付け外しすると、
	 * 描画コールバックの錠を要求のスレッドから取ることになる（「錠の順番」） */
	obs_add_main_rendered_callback(on_rendered, NULL);
	S.callback_added = true;
	ss_vendor_register_request(REQ_SUBSCRIBE, req_subscribe, NULL);
	ss_vendor_register_request(REQ_UNSUBSCRIBE, req_unsubscribe, NULL);
	ss_vendor_register_request(REQ_GET, req_get, NULL);
	S.enabled = true;
	obs_log(LOG_INFO, "program frames ready (canvases: %s)", C.ready ? "all" : "main only");
}

void program_frames_shutdown(void)
{
	/* 先に描画のコールバックを外す（外したあとは描画のスレッドが取り口を触らない） */
	if (S.callback_added) {
		obs_remove_main_rendered_callback(on_rendered, NULL);
		S.callback_added = false;
	}
	if (S.thread_started) {
		os_atomic_set_bool(&S.stopping, true);
		os_event_signal(S.wake);
		pthread_join(S.thread, NULL);
		S.thread_started = false;
	}
	if (S.wake) {
		os_event_destroy(S.wake);
		S.wake = NULL;
	}

	struct sub *dead[MAX_SUBS];
	size_t n = 0;
	pthread_mutex_lock(&S.mutex);
	while (S.count > 0)
		dead[n++] = sub_take_at(S.count - 1);
	pthread_mutex_unlock(&S.mutex);
	for (size_t i = 0; i < n; i++)
		sub_free(dead[i]);

	pthread_mutex_destroy(&S.mutex);
	S.enabled = false;
}

bool program_frames_available(void)
{
	return S.enabled;
}
