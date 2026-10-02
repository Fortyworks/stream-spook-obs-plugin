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
 * いま読み込まれている先が OBS Studio か Streamlabs Desktop かを見分ける。
 *
 * Streamlabs Desktop は libobs を独自にフォークしていて、OBS の SDK で組んだ
 * この DLL とは構造体の並びが一部食い違う（obs_source_info の audio_render の直後に
 * 1 つ挿し込まれている・obs_video_info の頭に 1 つ足されている）。フィルタは
 * 挿し込み位置より前の項目しか使わないのでそのまま動くが、スティンガーは後ろの
 * 項目（enum_all_sources / transition_start …）を使うので、ずれて別の関数として
 * 呼ばれて落ちる。読み込まれた先を見て、食い違いに当たるものは登録しない。
 */
#pragma once

#include <stdbool.h>

enum ss_host {
	SS_HOST_OBS,
	SS_HOST_STREAMLABS,
};

/* obs_module_load の頭で 1 回だけ呼ぶ */
void ss_host_detect(void);

enum ss_host ss_host(void);

/* "obs" / "streamlabs"（通信口の host_info で返す名前） */
const char *ss_host_name(void);
