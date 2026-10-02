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
#include "host.h"

#include <obs-module.h>

#include "plugin-support.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static enum ss_host host = SS_HOST_OBS;

void ss_host_detect(void)
{
	host = SS_HOST_OBS;
#ifdef _WIN32
	/* 版の文字列（"32.1.1sl12"）は作り方しだいで変わるので当てにしない。
	 * フォークにしか無い関数が obs.dll から引けるかで見る（obs-studio-node が
	 * キャンバスを複数持つために足したもの。OBS 本体には同じ名前が無い） */
	HMODULE obs_dll = GetModuleHandleW(L"obs.dll");
	if (obs_dll && GetProcAddress(obs_dll, "obs_get_video_info_count") != NULL)
		host = SS_HOST_STREAMLABS;
#endif
	obs_log(LOG_INFO, "host: %s (libobs %s)", ss_host_name(), obs_get_version_string());
}

enum ss_host ss_host(void)
{
	return host;
}

const char *ss_host_name(void)
{
	return host == SS_HOST_STREAMLABS ? "streamlabs" : "obs";
}
