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
 * オーバーレイ（ブラウザソース）だけでは届かないもの ― 配信画面そのものに
 * 掛けるポストエフェクトや、OBS の中を流れる音 ― を OBS の中で受け持つ。
 * アプリ本体からは obs-websocket で叩く前提（フィルタは SetSourceFilterSettings /
 * SetSourceFilterEnabled、音とスティンガーの設定は vendor API。spectrum.c /
 * stinger-transition.c）。
 *
 * Streamlabs Desktop（libobs のフォーク）にも同じ DLL のまま入る。そちらには
 * obs-websocket が無いので、vendor の要求は自前のパイプで運ぶ（vendor.c / pipe.c）。
 * 構造体の並びが食い違うスティンガーは登録しない（host.h）。
 */
#include <obs-module.h>
#include <plugin-support.h>

#include "host.h"
#include "meters.h"
#include "spectrum.h"
#include "stinger-transition.h"
#include "vendor.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char *obs_module_name(void)
{
	return "StreamSpook for OBS";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Post effects and helpers for StreamSpook";
}

extern struct obs_source_info sepia_filter_info;
extern struct obs_source_info kurosawa_filter_info;
extern struct obs_source_info mosaic_filter_info;
extern struct obs_source_info vignette_filter_info;
extern struct obs_source_info glitch_filter_info;
extern struct obs_source_info chromatic_filter_info;
extern struct obs_source_info grade_filter_info;
extern struct obs_source_info lens_filter_info;
extern struct obs_source_info skin_filter_info;

bool obs_module_load(void)
{
	ss_host_detect();
	obs_register_source(&sepia_filter_info);
	obs_register_source(&kurosawa_filter_info);
	obs_register_source(&mosaic_filter_info);
	obs_register_source(&vignette_filter_info);
	obs_register_source(&glitch_filter_info);
	obs_register_source(&chromatic_filter_info);
	obs_register_source(&grade_filter_info);
	obs_register_source(&lens_filter_info);
	obs_register_source(&skin_filter_info);
	stinger_register();

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

/* どこに読み込まれていて、何が使えるか。アプリはつないだら最初にこれを聞く
 * （Streamlabs Desktop ではスティンガーが無い、など）。
 *   → { version, host: "obs" | "streamlabs", libobs, features: { stinger, meters, spectrum } } */
static void req_host_info(obs_data_t *req, obs_data_t *res, void *priv)
{
	UNUSED_PARAMETER(req);
	UNUSED_PARAMETER(priv);
	obs_data_set_string(res, "version", PLUGIN_VERSION);
	obs_data_set_string(res, "host", ss_host_name());
	obs_data_set_string(res, "libobs", obs_get_version_string());
	obs_data_t *features = obs_data_create();
	obs_data_set_bool(features, "stinger", stinger_available());
	obs_data_set_bool(features, "meters", true);
	obs_data_set_bool(features, "spectrum", true);
	obs_data_set_obj(res, "features", features);
	obs_data_release(features);
}

/* obs-websocket の vendor API は、全モジュールが読み込まれたあとでしか使えない */
void obs_module_post_load(void)
{
	ss_vendor_init();
	if (ss_vendor_available())
		ss_vendor_register_request("host_info", req_host_info, NULL);
	spectrum_init();
	meters_init();
	stinger_init_vendor();
	/* 要求が全部そろってから口を開く（Streamlabs Desktop のときだけ） */
	ss_vendor_open_pipe();
}

void obs_module_unload(void)
{
	/* 先に口を閉じる（閉じたあとは要求が来ない）。それから各機能を畳む */
	ss_vendor_shutdown();
	spectrum_shutdown();
	meters_shutdown();
	stinger_shutdown();
	obs_log(LOG_INFO, "plugin unloaded");
}
