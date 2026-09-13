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
 * 掛けるポストエフェクトなど ― を OBS の中で受け持つ。
 * アプリ本体からは obs-websocket の SetSourceFilterSettings /
 * SetSourceFilterEnabled で叩く前提。
 */
#include <obs-module.h>
#include <plugin-support.h>

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

bool obs_module_load(void)
{
	obs_register_source(&sepia_filter_info);
	obs_register_source(&kurosawa_filter_info);
	obs_register_source(&mosaic_filter_info);
	obs_register_source(&vignette_filter_info);
	obs_register_source(&glitch_filter_info);
	obs_register_source(&chromatic_filter_info);
	obs_register_source(&grade_filter_info);
	obs_register_source(&lens_filter_info);

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "plugin unloaded");
}
