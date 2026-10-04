# StreamSpook for OBS

[StreamSpook](https://streamspook.app) が OBS の中で受け持つぶん ―― 配信画面そのものに掛けるポストエフェクトや、OBS の中を流れる音など、ブラウザソースのオーバーレイでは届かないもの ―― のネイティブプラグイン。モジュール名は `stream-spook`、土台は [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate)。

- **ライセンス: GPL-2.0-or-later**（[LICENSE](./LICENSE)）。libobs が GPL-2.0-or-later で、そのヘッダのコード（マクロ・`static inline`）がこの DLL に直接含まれるため。StreamSpook 本体とは別のプログラムで、話すのは obs-websocket の JSON だけ
- **公開しているのは [Fortyworks/stream-spook-obs-plugin](https://github.com/Fortyworks/stream-spook-obs-plugin)。** リリースごとのスナップショット（1 版 = 1 コミット）で、開発の履歴と PR は入っていない。開発は非公開のリポジトリで行い、版を上げて main へマージすると Actions がタグを打ってミラーへ積む（「8. 公開リポジトリ（ミラー）」）
- **いま入っているもの:** ポストエフェクト 8 種（古い映画 / 黒澤モード / モザイク / ビネット / グリッチ / 色収差 / カラーグレーディング / レンズのゆがみ）と、オーディオビジュアライザー用の音の取り口（トラックのミックス / ソース 1 つ。「3. オーディオビジュアライザー」）、シーントランジション「StreamSpook: スティンガー」（「4. スティンガー」）
- **配り方:** Release の zip を StreamSpook 本体が同梱し、アプリの「OBS プラグイン」ページから OBS のユーザー用プラグインフォルダへ入れる。手で入れることもできる（下）
- **Streamlabs Desktop にも同じ DLL のまま入る。** フィルタと音の取り口はそのまま動き、スティンガーだけ登録しない。obs-websocket が無いので、話すのは自前の名前付きパイプ（「5. Streamlabs Desktop」）

## 1. ビルド（Windows）

必要なもの: Visual Studio 2022（C++ ツール）、CMake 3.28 以上（VS 同梱のもので可）、Windows 10 SDK 10.0.22621 以上。

```powershell
cmake --preset windows-x64          # 初回は OBS のソースと依存を .deps に落として libobs を組む（数分）
cmake --build --preset windows-x64
node tools/package.mjs              # release/ と stream-spook-<版>-windows-x64.zip（+ .sha256）
```

| もの | 場所 |
|---|---|
| DLL | `build_x64/RelWithDebInfo/stream-spook.dll` |
| データ（effect / locale） | `build_x64/rundir/RelWithDebInfo/stream-spook/` |
| 配布物 | `release/`（`manifest.json` + `stream-spook/`）と zip |

依存の版は `buildspec.json` が決める（OBS 31.1.1 の SDK でビルド）。macOS / Linux の preset は obs-plugintemplate のまま残してあるが**未ビルド・未確認**。

### 手で OBS に入れる

OBS が Windows で探す置き場所（`%ProgramData%\obs-studio\plugins\<名前>\`。OBS 本体の `AddExtraModulePaths` は Windows では `GetProgramDataPath` を使う。**`%APPDATA%` は読まない**）へ:

```powershell
cmake --install build_x64 --config RelWithDebInfo --prefix "$env:ProgramData\obs-studio\plugins"
```

置かれるのは `%ProgramData%\obs-studio\plugins\stream-spook\bin\64bit\stream-spook.dll` と `…\stream-spook\data\`。`ProgramData` の直下はふつうのユーザーでもフォルダを作れるので、管理者権限はたいてい要らない。**OBS を再起動**すると、プラグインマネージャーに「StreamSpook for OBS」が、フィルタ一覧に「StreamSpook: …」が 8 つ並ぶ。外すときはこのフォルダごと消す。

- OBS が起動中だと DLL の上書きに失敗する（初回の新規コピーは通る）
- ポータブルモードの OBS は `ProgramData` を見ないので、この置き場所では読まれない
- Streamlabs Desktop は置き場所が違う（「5. Streamlabs Desktop」）

### OBS を起動せずに確かめる（smoke）

```powershell
pwsh tools/smoke/Run-Smoke.ps1
```

`.deps` の OBS ソースから `libobs-d3d11` を組み、プラグインを libobs に読み込んで全フィルタを作り（＝ `.effect` を実際にコンパイルし）、2 色のソースに掛けてピクセルを読み戻す（座標をずらすものは境目の色が動くこと、樽型のゆがみは角が透明になることまで見る）。終了コード 0 で OK。シェーダーのエラーは `[obs 300]` 以下の行に出る。`.effect` を触ったら必ず回す。

オーディオビジュアライザー（`src/spectrum.c`）も同じ smoke が見る。obs-websocket は読み込まないので、vendor API の偽物（中身は proc_handler の呼び出しだけ）を先に置いてから、1kHz の正弦波を出すソースを購読し、イベントの帯が正しい位置に立つこと・`unsubscribe` で消えること・期限（6 秒）が切れると止まること・消したソースは受けないことを見る。スティンガー（`src/stinger-transition.c`）は、OBS の画面と同じく private のソースとして作り、`stinger_configure` で設定（行き先ごとの `scenes` を含む）が届いて保存されること・読み直したものは上書きしないこと・長さを固定していないこと・OBS の長さ（1500ms）で回したとき、行き先ごとの設定がある行き先はその切り替え点で、無い行き先は既定の切り替え点で A / B が替わること・ページへ投げる detail（OBS の長さを測った `durationMs`、行き先の比の `pointMs`、`sceneUuid`、`elapsedMs`）・T バー（手動）では行き先の `duration_ms` を使うこと・中のブラウザのモニタリングが既定で「なし」で、`monitoring` の 3 つがそのまま届き、知らない値・省いたとき・持たない保存は「なし」になることを見る（モニタリングは最後に偽の `browser_source` を登録して確かめる）（obs-browser は読み込まないので、ページの絵そのものはここでは出ない。detail はスティンガーの proc `stinger_last_event` から読む。読むだけの口）。帯の計算そのもの（`spectrum-analyzer.c`）は libobs 無しの `spectrum-analyzer-test`（CMake の target。配布物には入らない）が先に回る。CI でも回る。

音量メーター（`src/meters.c`）と、どこに読み込まれたか（`host_info`）も同じ smoke が見る。0.5 の正弦波で peak がおよそ 0.5 になること・購読を外すと消えることまで。

```powershell
pwsh tools/smoke/Run-Smoke.ps1 -Streamlabs
```

`-Streamlabs` を付けると、**同じ DLL を Streamlabs Desktop の libobs（独自のフォーク）に読み込んで**回す。Streamlabs が配っている libobs 一式（ヘッダ・`obs.lib`・DLL）を `tools/smoke/streamlabs.json` の版と sha256 で `.deps` に落とし（約 155MB、初回だけ）、smoke.exe だけをフォークのヘッダで組み直す。プラグインは組み直さない（配る DLL 1 つで両方に入る、を確かめるのが目的）。フィルタ・音の取り口・音量メーターに加えて、スティンガーを登録していないこと、パイプにつないで要求とイベントが通ること、切ってつなぎ直せることを見る。Streamlabs が libobs を上げたら `streamlabs.json` を上げて回す（版は obs-studio-node の `.github/workflows/main.yml` の `LibOBSVersion`）。

### 文言の見張り

```powershell
node tools/check-locale.mjs
```

C が引いているキーが `data/locale/en-US.ini` と `ja-JP.ini` の両方にあるか、フィルタ ID の付け方（`stream_spook_*`）、フィルタ名（`StreamSpook: …`）を見る。CI でも回る。

## 2. フィルタと設定キー

設定キーはそのまま obs-websocket の `filterSettings` の名前。値の範囲は OBS の設定画面のスライダーの端で、それより外は丸めていない。

### 共通

| キー | 型 | 意味 |
|---|---|---|
| `strength` | 0..1 | 効果ぜんたいの混ぜ具合。0 で素通し（描画もしない） |
| `transition_ms` | 0..5000 | 値を変えたとき、その値に落ち着くまでの時間。アプリから動かすときの主役 |
| `anim_mode` | 0 なし / 1 脈打つ / 2 ランダム | 「揺らす」動き。何を揺らすかはフィルタごと（下の表） |
| `anim_amount` | 0..1 | 揺らす振れ幅（値の単位そのまま） |
| `anim_period` | 0.05..10 秒 | 脈の 1 周期、ランダムの 1 区間 |

`transition_ms` はスライダーの値をなめらかに移すだけの仕組みなので、アプリ側は「`strength: 1, transition_ms: 300`」→ しばらくして「`strength: 0, transition_ms: 1500`」の 2 回叩けば、立ち上がって消えていく演出になる。毎フレーム値を送る必要はない。

### 古い映画（セピア） `stream_spook_sepia`

揺らす対象: `damage`（乱れの量）

| キー | 範囲 | 意味 |
|---|---|---|
| `fade` | 0..1 | 黒の浮き（退色） |
| `vignette` | 0..1 | 周辺の暗さ |
| `damage` | 0..1 | 乱れの総量 |
| `grain` / `scratches` / `dust` / `flicker` / `jitter` | 0..1 | 乱れの内訳（粒子 / 縦の傷 / ゴミ / 明るさのちらつき / コマの揺れ） |
| `speed` | 0.25..4 | 乱れの速さ |

### 黒澤モード（モノクロ映画） `stream_spook_kurosawa`

揺らす対象: `damage`。乱れのキーはセピアと同じ。

| キー | 範囲 | 意味 |
|---|---|---|
| `contrast` | 0.5..3 | コントラスト |
| `blacks` | 0..1 | 黒つぶし |
| `vignette` | 0..1 | 周辺の暗さ |

### モザイク `stream_spook_mosaic`

揺らしは無し（`transition_ms` だけ効く。ブロックの大きさをなめらかに変える）。

| キー | 範囲 | 意味 |
|---|---|---|
| `block_size` | 1..200 | ブロックの大きさ（px）。1 で素通し。200 はスライダーの端で、websocket からはそれより大きくても効く |
| `region_enabled` | bool | 一部にだけ掛ける |
| `region_x` / `region_y` / `region_w` / `region_h` | px | 範囲。**フィルタを掛けたソースのピクセル**（シーンに掛けたらキャンバス） |
| `region_shape` | 0 四角 / 1 楕円 | 形 |
| `region_feather` | 0..200 px | ふちのぼかし |
| `region_invert` | bool | 範囲の外に掛ける |

### ビネット `stream_spook_vignette`

揺らす対象: `strength`

| キー | 範囲 | 意味 |
|---|---|---|
| `radius` | 0..1.5 | 暗くなり始める位置（中心 0、端 1） |
| `softness` | 0..1.5 | 暗くなりきるまでの幅 |
| `roundness` | 0..1 | 0 で四角、1 で楕円 |
| `color` | ABGR 整数 | 寄せる色（既定 `0xFF000000` = 黒） |

### グリッチ `stream_spook_glitch`

揺らす対象: `strength`

| キー | 範囲 | 意味 |
|---|---|---|
| `frequency` | 0..1 | 乱れの波が来る頻度。1 で常に乱れっぱなし |
| `slices` / `rgb_split` / `blocks` / `noise` | 0..1 | 乱れの内訳（横帯のずれ / 色ずれ / ブロックの飛び / 砂嵐） |
| `speed` | 0.25..4 | 乱れの速さ |

### 色収差 `stream_spook_chromatic`

揺らす対象: `strength`。R と B を G からずらして拾う。

| キー | 範囲 | 意味 |
|---|---|---|
| `radial` | 0..1 | 端に向かうずれ（中心からの距離に比例。角で最大 3%） |
| `shift` | 0..1 | 一方向のずれ（画面幅の最大 1%） |
| `angle` | 0..360 | 一方向のずれの向き（度。0 で右、90 で下） |

### カラーグレーディング `stream_spook_grade`

揺らす対象: `strength`（元の絵との混ぜ具合）。順番はルック → 露出 → ホワイトバランス → コントラスト → 彩度 → スプリットトーン。

| キー | 範囲 | 意味 |
|---|---|---|
| `look` | 0 なし / 1 ティール＆オレンジ / 2 暖色 / 3 寒色 / 4 ブリーチバイパス / 5 ヴィンテージ | 決め打ちの色の作り（出発点） |
| `look_amount` | 0..1 | ルックの強さ |
| `exposure` | -2..2 | 露出（EV。1 で 2 倍） |
| `contrast` | 0.5..2 | コントラスト |
| `saturation` | 0..2 | 彩度 |
| `vibrance` | -1..1 | 自然な彩度（彩度の低い色ほど効く） |
| `temperature` | -1..1 | 色温度（正で暖かく） |
| `tint` | -1..1 | 色かぶり（正でマゼンタ、負で緑） |
| `shadow_color` / `shadow_amount` | ABGR / 0..1 | 暗部に寄せる色と量 |
| `highlight_color` / `highlight_amount` | ABGR / 0..1 | 明部に寄せる色と量 |
| `balance` | -1..1 | 暗部と明部の境目 |

### レンズのゆがみ `stream_spook_lens`

揺らす対象: `amount`。

| キー | 範囲 | 意味 |
|---|---|---|
| `amount` | -1..1 | ゆがみ。正で樽型（辺が外へふくらむ）、負で糸巻き型。0 で素通し |
| `dispersion` | 0..1 | 色ごとにゆがみをずらす（色収差） |
| `fit` | bool | 樽型のときに角が元の外を指さないよう内側へ寄せる（既定オン） |
| `transparent` | bool | 元の外を指した所を透明にする（偽なら端の色を伸ばす。既定オン） |

### 美肌 `stream_spook_skin`

揺らす対象: `strength`（元の絵との混ぜ具合）。カメラのソースに掛ける想定。

肌の範囲は色だけで決める（YCbCr の Cb / Cr が肌の色の楕円に入っているか。顔の検出はしない）。肌に近い色の背景（木の壁・ベージュの服）も一緒に拾うので、範囲の広さと境目のなだらかさで合わせる。合わせるときは `show_mask` で範囲を白黒で見る。順番は 範囲を決める → なめらかにする → 色調補正 → 元の絵と混ぜる。

| キー | 範囲 | 意味 |
|---|---|---|
| `whiten` | 0..1 | 色調補正（中間の明るさを持ち上げ、赤みを抜く）の強さ |
| `redness` | 0..1 | 色調補正のうち、赤みを抜く割合 |
| `smooth_enabled` | bool | なめらかにするか（OBS の設定画面ではチェック付きの見出し） |
| `smooth_strength` | 0..1 | なめらかさ（輪郭を残すぼかし。強いほど色の離れた点まで混ぜる） |
| `smooth_radius` | 1..20 px | ぼかす範囲 |
| `skin_range` | 0..1 | 肌とみなす色の範囲の広さ |
| `skin_softness` | 0..1 | 範囲の境目のなだらかさ |
| `show_mask` | bool | 肌とみなした範囲を白黒で出す（合わせるとき用。効きが 0 でも描く） |

`whiten` と `smooth_strength` も `transition_ms` で移る（揺らしはしない）。`smooth_enabled` のオン / オフも、なめらかさを 0 へ移す形でつなぐ。`whiten` と（なめらかさがオフか 0 の）両方が 0 のフレームは素通しにする。

## 3. オーディオビジュアライザー（音の取り口）

StreamSpook 本体のカスタムオーバーレイにあるビジュアライザーは、既定では PC 全体の音（WASAPI のループバック）を本体が拾う。このプラグインが入っていると、**OBS の中を流れる音**を選べる。obs-websocket が流してくるのは音量（`InputVolumeMeters`）だけで周波数の成分が無いので、libobs の中で PCM を受けて FFT にかけ、帯の強さだけを外へ出す。

| 鍵（key） | 取り口 | 何の音か |
|---|---|---|
| `track-<1..6>` | `obs_add_raw_audio_callback` | そのトラックのミックス。トラック 1 はふつう「配信へ出て行く音ぜんぶ」 |
| `input-<uuid>` | `obs_source_add_audio_capture_callback` | ソース 1 つ。そのソースのフィルタを通ったあと、フェーダーの手前の音。ミュート中は無音。フィルタを付けて回る必要は無い |

話し方は obs-websocket の **vendor API**（vendor 名 `stream-spook`）。本体は既に obs-websocket につながっているので、経路を増やさない。

| 種類 | 名前 | 中身 |
|---|---|---|
| 要求 | `spectrum_sources` | → `{ version, sampleRate, tracks, inputs: [{ uuid, name, kind }] }`。音を持つ入力ソースの一覧（シーンは含まない） |
| 要求 | `spectrum_subscribe` | `{ keys: [{ key }] }` → `{ active: [{ key }], rejected: [{ key, reason }] }`。`reason` は `unknown`（無い・消された・鍵の形が違う）/ `not_audio` / `full`。**呼ぶたびに期限が 6 秒延びる。** 本体は見ている絵があるあいだ 2 秒おきに呼び続け、本体が落ちても数秒で止まる |
| 要求 | `spectrum_unsubscribe` | `{ keys: [{ key }] }` → `{}`。すぐ畳む |
| イベント | `spectrum` | `{ taps: [{ k: key, b: "12,48,90,…" }] }`。毎秒 30 回。`b` は 0..255 が 64 個のコンマ区切り |

- **配列の中身はオブジェクトだけ。** `obs_data` の配列は文字列や数を持てない（JSON から読むときに落ちる）ので、鍵は `{ key }` で包み、帯は文字列で載せる
- **帯の切り方は本体の PC 音の経路と同じ**（2048 点・64 帯・30Hz〜16kHz の対数・底 -70dB・+2.5dB/oct の持ち上げ）。切り替えても同じ曲で棒の高さが変わらないようにするため。値を変えるときは本体の `src-tauri/src/spectrum.rs` と対で直す
- **無音は 1 回だけ流す。** 全部 0 の配列を送り続けない。音が来ないまま止まっている取り口は 120ms で 0 を詰めて落とす
- **誰も見ていないときは動かない。** 期限が切れた取り口は畳み、取り口が 1 つも無いあいだスレッドは 100ms おきに起きて何もしない
- **Streamlabs Desktop では同じ要求とイベントが名前付きパイプを通る**（「5. Streamlabs Desktop」）
- **`obs-websocket-api.h` は obs-websocket のリポジトリから写したもの**（GPL-2.0-or-later、`src/obs-websocket-api.h`）。リンクは要らない（proc_handler を引くだけ）。obs-websocket が無ければ `obs_websocket_register_vendor` が NULL を返すので、フィルタだけで動く

## 4. スティンガー（シーントランジション）

`stream_spook_stinger`。OBS 標準のスティンガーと同じく、切り替えのあいだ前のシーン → 切り替え点で次のシーンを描き、その上に絵を重ねる。**重ねる絵は動画ファイルではなくブラウザソース**で、StreamSpook 本体のオーバーレイサーバーが配るページを開く。どんな絵を出すか（演出の種類・色・文字・音）は全部そのページと本体が決め、ここは「いつ流すか」「どこで替えるか」だけを受け持つ。

- **ブラウザソースは作ったときに 1 つだけ作り、ずっと読み込んだままにする**（`shutdown` オフ＋ `obs_source_inc_showing`）。切り替えが始まり、行き先のシーンが決まったら、obs-browser の `javascript_event`（proc_handler）でページへイベント `streamspook:stinger` を投げる（detail は下の表）。毎回ページを読み直さないので、読み込みの待ちが切り替えに乗らない
- **音はブラウザの音を OBS へ回し（`reroute_audio`）、切り替えの音として配信に乗せる。** シーンの音は標準のスティンガーの「フェードアウト→フェードイン」と同じ（切り替え点までに前を絞り、切り替え点から次を上げる）
- **配信者の耳にも鳴らせる（0.8.0〜）。** トランジションは音声ミキサーに出ないので、そのままでは配信には乗っても配信者には聞こえない。標準のスティンガーの「音声モニタリング」と同じく、中のブラウザにモニタリングを掛けて、OBS の「設定 → 音声 → モニタリングデバイス」でも鳴らす。3 つとも選べるのは、音の組み方で正解が違うため — デスクトップ音声がモニタリングデバイスと同じ機器を録っている（どちらも「既定」のままがこれ）なら「モニターのみ」（「モニターと出力」だと、モニターの音をデスクトップ音声がもう一度拾って配信に 2 回乗る）、別の機器なら「モニターと出力」。既定は標準のスティンガーと同じ「なし」（いまの配信の音を変えない）
- **長さは OBS が持つ**（「期間」欄と、シーンごとの「トランジションの上書き」の期間）。固定の長さ（`obs_transition_enable_fixed`）は使わないので、OBS の「期間」欄が出る。本体は行き先のシーンごとに別の演出（長さ・切り替え点）を選べるが、OBS に置くスティンガーは 1 つで、libobs は `transition_start` を行き先を決める**前**に呼んで、その直後に固定の長さを読む。行き先ごとに固定の長さを切り替える隙も、途中で切り上げる公開 API も無いので、長さは本体が OBS に書き込む（既定は「期間」欄、シーンごとは `SetSceneSceneTransitionOverride`）
- **行き先ごとの切り替え点はプラグインが選ぶ。** 行き先（B）が決まったら（自分の `transition_start` シグナル。libobs が B を置いたあとに出る）、次の `video_tick` でその uuid を `scenes` から探し、無ければ既定の値を使う。切り替え点は `point_ms / duration_ms` の比で、OBS の長さに掛けて使う（0.001..0.999 に丸める）。決まるまでの 1 フレームは前のシーンのまま
- **実際の長さは測る。** libobs の t（0..1）と同じ時計で 2 フレーム見て「開始からの経過 / t」と「フレーム間の経過 / t の伸び」が合えば、それが OBS の長さ。合わない（スタジオモードの T バー。libobs に今のモードを読む口が無い）・50..60000ms の外・t が 0.5 秒動かない、のときはその行き先の `duration_ms` を使う
- **OBS の一覧に足すのは配信者。** obs-websocket にも frontend API にもトランジションを足す口が無いので、「シーントランジション」の ＋ から 1 回だけ足してもらう。OBS 側のプロパティは案内の文だけ（`get_properties` が無いと ＋ の一覧に出てこない）
- **本体が落ちているとき**はページが読めないので、絵の無いカットになる（切り替え点で替わるだけ）

設定（`stinger_configure` で届いたものをそのまま持つ。シーンコレクションと一緒に保存される）:

| キー | 型 | 意味 |
|---|---|---|
| `url` | string | 開くページ |
| `duration_ms` | 100..20000 | 既定の演出の長さ（`scenes` に無い行き先に使う）。切り替え点の比を出すのと、長さを測れないときの代わりに使う |
| `point_ms` | 0..duration_ms | 既定の切り替え点（前のシーンから次のシーンへ替える位置。ページの絵が画面を覆っているところ） |
| `scenes` | `[{ uuid, duration_ms, point_ms }]` | 行き先のシーン（uuid）ごとの長さと切り替え点。範囲は上と同じ。uuid の無いもの・重なった uuid の 2 つ目は落とす（最大 256 件） |
| `monitoring` | `"none"` \| `"monitor_only"` \| `"monitor_and_output"` | 中のブラウザの音声モニタリング。既定・知らない値・0.7.0 までに保存されたもの（このキーが無い）は `none` |

| 種類 | 名前 | 中身 |
|---|---|---|
| 要求 | `stinger_configure` | `{ url, duration_ms, point_ms, scenes?: [{ uuid, duration_ms, point_ms }], monitoring?: "none" \| "monitor_only" \| "monitor_and_output" }` → `{ count, per_scene: true, monitoring: true }`。いまあるスティンガー全部に同じ設定を配り、このあと ＋ から作られるぶん（OBS を再起動するまで）もこの設定で始める。シーンコレクションから読み直したもの（自分の URL を持っている）は上書きしない。`scenes` を省くと空（前に配ったぶんは残さない）。`per_scene` は行き先ごとの切り替え点を知っている版（0.5.0〜）の目印。`monitoring` を省くと `none`（モニタリングを知らない本体からの要求で、配信の音を変えない）。返事の `monitoring` はモニタリングを知っている版（0.8.0〜）の目印。本体はつなぐたびに呼ぶ |
| ページへのイベント | `streamspook:stinger` | detail は `{ durationMs, pointMs, sceneUuid, elapsedMs }`。`durationMs` は OBS の実際の長さ（測れないときは行き先の `duration_ms`）、`pointMs` はそれに切り替え点の比を掛けたもの、`sceneUuid` は行き先のシーンの uuid（`scenes` に無くても入る。行き先が無いときは空）、`elapsedMs` は投げた時点で OBS の切り替えが進んでいる量（行き先を決めて長さを測るのに 1〜2 フレーム掛かるので、ページはそのぶん先へ送って OBS と揃える）。切り替えの途中でまた切り替えられたら、決め直してもう一度投げる |

- **vendor は 1 本（`src/vendor.c`）。** 同じ名前の vendor は 1 度しか登録できないので、音の取り口とスティンガーが同じ 1 本に要求を足す
- **Streamlabs Desktop では登録しない**（「5. Streamlabs Desktop」）

## 5. Streamlabs Desktop

Streamlabs Desktop は libobs を独自にフォークして使っている（obs-studio-node。いまは `32.1.1sl12`）。このプラグインは**同じ DLL のまま**そちらにも入る。

| もの | Streamlabs Desktop で |
|---|---|
| フィルタ 9 種 | そのまま動く |
| 音の取り口（`spectrum_*`） | そのまま動く（パイプ経由） |
| 音量メーター（`meters_*`） | Streamlabs のためにある（下） |
| スティンガー | **登録しない** |

### 置き場所

Streamlabs Desktop がユーザーのプラグインを読むのは `%APPDATA%\slobs-plugins\` の下だけ（`app/services/obs-user-plugins.ts` が起動時にフォルダを作り、obs-studio-node の `addModulePaths()` が読む）。OBS の `%ProgramData%\obs-studio\plugins` は読まない。並びも OBS と違う:

| もの | 置き場所 |
|---|---|
| DLL | `%APPDATA%\slobs-plugins\obs-plugins\64bit\stream-spook.dll` |
| データ（effect / locale） | `%APPDATA%\slobs-plugins\data\obs-plugins\stream-spook\` |

Streamlabs を再起動すると読まれる。Mac 版は同梱のプラグインしか読まないので対象外。Streamlabs の画面のフィルタ一覧は許可リストなので、**ここのフィルタは Streamlabs の画面には出ない**（アプリから付ける）。

### スティンガーを登録しない理由

フォークは `obs_source_info` の `audio_render` の直後に `audio_render_do` を 1 つ挿し込んでいて、OBS の SDK で組んだこの DLL とは、それより後ろの項目が 1 つずつずれる。フィルタが使う項目は全部それより前なので影響しないが、スティンガーは後ろの `enum_all_sources` / `transition_start` / `transition_stop` / `video_get_color_space` を使うので、別の関数として呼ばれて**作った瞬間に落ちる**（`obs_video_info` も頭に項目が足されていて、`obs_get_video_info` が受け皿の外まで書く）。Streamlabs の画面はプラグインのトランジションを選ばせもしないので、登録しない。

見分け方は `src/host.c`。フォークにしか無い関数（`obs_get_video_info_count`）が `obs.dll` から引けるかで見る（版の文字列は作り方しだいで変わるので当てにしない）。**フォークの構造体の並びが変わったら、ここに書いたことを見直す。** smoke の `-Streamlabs` が落ちるのがその合図。

### 話し方（名前付きパイプ）

Streamlabs Desktop には obs-websocket が入っていないので、**Streamlabs で読み込まれたときだけ**自前の口 `\\.\pipe\stream-spook` を開く（`src/pipe.c`）。OBS Studio では開かない（obs-websocket の 1 本で足りるので、通信路を増やさない）。要求とイベントは vendor と同じもので、各機能は口を意識しない（`src/vendor.c` が開いている口の全部に配る）。

1 行 1 つの JSON（UTF-8、改行で区切る）:

| 向き | 形 |
|---|---|
| 要求 | `{ "id": 1, "type": "meters_subscribe", "data": { … } }` |
| 応答 | `{ "id": 1, "data": { … } }`。知らない要求は `{ "id": 1, "error": "unknown_request" }` |
| イベント | `{ "event": "meters", "data": { … } }` |

- この PC の中からだけつなげる（`PIPE_REJECT_REMOTE_CLIENTS`）。同時に 4 本まで
- 読まずに溜めているつなぎ手は、書き込みが 250ms 詰まった時点で切る（音の受け取りを止めない）
- 1 行が 1MB を超えたら壊れた入力として切る

### 要求の一覧（どちらの口でも同じ）

| 種類 | 名前 | 中身 |
|---|---|---|
| 要求 | `host_info` | → `{ version, host: "obs" \| "streamlabs", libobs, features: { stinger, meters, spectrum } }`。アプリはつないだら最初にこれを聞いて、使えないもの（Streamlabs のスティンガー）を出さない |
| 要求 | `meters_subscribe` | `{ keys: [{ key: "input-<uuid>" }] }` → `{ active, rejected }`。鍵と期限（6 秒）の扱いは `spectrum_subscribe` と同じ |
| 要求 | `meters_unsubscribe` | `{ keys: [{ key }] }` → `{}` |
| イベント | `meters` | `{ inputs: [{ k, levels: [{ m, p, i }] }] }`。毎秒 20 回（obs-websocket の `InputVolumeMeters` と同じ 50ms おき）。`levels` はチャンネルごと、値は倍率（0..1）で、`m` = magnitude（RMS）・`p` = peak・`i` = フェーダーを通る前の peak。新しい値が届いた取り口だけを載せる |

- **音量メーターは Streamlabs のためにある。** OBS Studio では obs-websocket の `InputVolumeMeters` が同じものを流すので、本体はそちらを使う（要求は OBS でも登録されるが、呼ばれない）。Streamlabs Desktop の API には音量を外へ出す口が無く、音量ミキサーの提案・自動調整、マイク調整の計測、無言アラートがこれを読む
- **取り方は libobs の volmeter**（OBS のミキサーの棒と同じもの。`OBS_FADER_LOG`）。ソースの一覧は `spectrum_sources` を使う。Streamlabs のソース ID は libobs のソース名そのもの（`InputFactory.create(type, id, …)`）なので、本体は名前で突き合わせられる

## 6. つくり

```
buildspec.json        名前・版・依存の版（版を上げるのはここだけ。Release のタグと一致させる）
CMakeLists.txt        obs-plugintemplate そのまま＋ソース一覧
cmake/                obs-plugintemplate の補助（触らない。GPLv2）
src/
  plugin-main.c       登録だけ
  fx-common.h         effect の読み込み・対象の大きさ・時間の進め方
  fx-anim.{h,c}       「強さ」を動かす共通部品（移り変わり＋揺らし）と、その設定欄
  film-filter.c       セピアと黒澤（1 つの effect の technique 違い）
  mosaic-filter.c / vignette-filter.c / glitch-filter.c
  chromatic-filter.c / grade-filter.c / lens-filter.c / skin-filter.c
  spectrum.{h,c}      オーディオビジュアライザー用の音の取り口（vendor API・購読・期限）
  spectrum-analyzer.{h,c}  帯の計算（libobs に依存しない。単体で確かめられる）
  stinger-transition.{h,c} シーントランジション「スティンガー」（ブラウザソースを重ねる）
  meters.{h,c}        音量メーター（Streamlabs Desktop 向け。vendor の要求・購読・期限）
  host.{h,c}          読み込まれた先が OBS か Streamlabs Desktop かを見分ける
  vendor.{h,c}        アプリと話す口をまとめる（obs-websocket の vendor と、Streamlabs ではパイプ）
  pipe.{h,c}          Streamlabs Desktop で開く名前付きパイプ（\\.\pipe\stream-spook）
  obs-websocket-api.h obs-websocket の vendor API（向こうのリポジトリの写し）
data/
  effects/*.effect    描き方そのもの（HLSL 風の OBS effect）
  locale/{en-US,ja-JP}.ini
tools/
  package.mjs         配布物（release/ と zip）を作る
  check-locale.mjs    文言の突き合わせ
  smoke/              OBS を起動せずに読み込んで確かめる道具（analyzer-test.c は帯の計算だけを見る。streamlabs.json は -Streamlabs で読み込む libobs の版）
```

決めごと:

- **描き方は `.effect` に置き、C 側は値を渡すだけ。** 乱数もテクスチャを持たず、時間と座標のハッシュで作る。設定画面のプレビューと配信画面で同じ絵になる
- **「強さ」の動かし方は `fx-anim` の 1 か所。** 移り変わり（`transition_ms`）も揺らし（`anim_*`）もここ。フィルタごとに書き起こさない。キーの名前を全フィルタでそろえてあるのは、アプリ側がフィルタの種類を見ずに同じキーで叩けるようにするため
- **フィルタ ID は `stream_spook_` で始める。** obs-websocket の `GetSourceFilterKindList` で「入っているか」を見分ける目印。設定キーは一度決めたら変えない（アプリ側が名指しで叩く）
- **文言は `en-US.ini` と `ja-JP.ini` の両方に入れる。** OBS は無いキーをそのまま画面に出す。フィルタ名は「StreamSpook: 〜」で始める
- **SDR 前提（`OBS_SOURCE_SRGB` を付けていない）。** シェーダーはガンマ空間の値をそのまま扱う。見た目を作るエフェクトはこのほうが素直で、Twitch も SDR。HDR のソースに掛けると色が飛ぶが、いまは対象外
- **`OBS_NO_DIRECT_RENDERING`。** 隣のピクセルを見る（モザイク・グリッチ）ので、対象を一度テクスチャに描かせてから掛ける
- **シェーダーは D3D11 と GLSL の両方で通る書き方にする。** OBS は macOS / Linux で HLSL 風の effect を GLSL に自動変換する。`for` と `int → float` のキャストは避けて関数を並べる、`half` / `line` / `round` / `noise` のような予約語を変数名に使わない、数の掛け算は小数リテラル（`2.0`）で書く
- **時間は 1000 秒で折り返す**（`fx_advance_time`）。float の精度が落ちる前に戻す
- **`strength` が 0 のフレームは `obs_source_skip_video_filter` で素通し。** 掛けていないのに GPU を食わない
- **音は取るだけで、描かない。** 帯の強さを出すところまでがここの仕事で、棒の描き方・追従・色は本体のオーバーレイが持つ。鍵の形（`track-<n>` / `input-<uuid>`）とイベントの形は一度決めたら変えない（本体が名指しで叩く）
- **秘密にしたいものを置かない。** このリポジトリは GPL で全部公開される。独自のロジックは StreamSpook 本体に置き、ここは「OBS の中でしかできないこと」を薄く受け持つ。本体のコードをここへ写さない（GPL になる）し、ここのコードを本体へ写さない（本体が GPL の派生物になる）

## 7. リリース

### 版の付け方

版は **`<本体の major>.<本体の minor>.<X>`**。X は対応する StreamSpook 本体の major.minor の中でこのプラグインを出した回数で、出すたびに 1 つ上げる。

- 本体が 1.0.x のあいだに出す版は 1.0.0 → 1.0.1 → 1.0.2 …。本体のパッチではこちらの版は動かさない
- **本体の minor / major が上がったら、こちらに変更が無くても `<major>.<minor>.0` で出し直す**（本体 1.1.0 → プラグイン 1.1.0）。X は 0 に戻る。版の頭 2 桁で、どの本体と組みになっているかが分かるようにするため
- 0.x（0.8.0 まで）はこの決まりを作る前の版。本体 1.0 のあいだは 0.x のまま出していたので 1.0.x は飛ばし、最初の版は本体 1.1.0 と組みになる 1.1.0
- 本体側は `package.json` と `src-tauri/obs-plugin.json` の頭 2 桁がそろっているかをテストで見ている（本体の `docs/obs-plugin.md`「プラグインの版の付け方」）

### 手順

1. `buildspec.json` の `version` を上の付け方で上げた PR を作る
2. main へマージすると、GitHub Actions がビルドして、`v<版>` のタグが無ければそのコミットに打ち、Release を作る（`stream-spook-<版>-windows-x64.zip` と `.sha256`）。版を上げずにマージしたときは、タグが既に別のコミットにあるので何もしない
3. 手でタグを打っても同じく Release ができる（`git tag v1.0.0 && git push origin v1.0.0`。版と違う番号なら止まる）。main 以外のコミットから出し直したいときだけ使う
4. StreamSpook 本体の `src-tauri/obs-plugin.json` に版と sha256 を書く。本体のリリースがその zip を取って同梱する

`manifest.json`（`{ "version": "…" }`）と `stream-spook/` の並びは本体の `src-tauri/src/obs_plugin.rs` が読む契約なので、形を変えるときは両方を直す。

## 8. 公開リポジトリ（ミラー）

GPL の「ソースを渡す」義務は、公開用のミラー [Fortyworks/stream-spook-obs-plugin](https://github.com/Fortyworks/stream-spook-obs-plugin) で満たす。こちら（開発用）は非公開のままで、履歴・PR・Actions の実行はここにしか残らない。

Release を作った実行（main への push で版が新しかったとき、またはタグ `v*` の push）では、`build.yml` の `mirror` ジョブが:

1. そのタグのツリーを `git archive` で取り出し（`.gitattributes` の `export-ignore` でワークフローは外す）、ミラーの `main` に **1 コミット**（メッセージは版の番号）として積んで、同じ番号のタグを打つ。作者は 1 つのアカウント（既定は the40san。メールは GitHub の noreply アドレス）にそろえるので、ミラーの履歴に他の名前は出ない。変えるならリポジトリ変数 `MIRROR_COMMIT_NAME` / `MIRROR_COMMIT_EMAIL`
2. こちらの Release と**同じ zip / .sha256 をそのまま**ミラーの Release に置く（作り直さない。本体が固定している sha256 が、どちらから取っても一致するように）

要るもの（このリポジトリの Settings）:

| 種類 | 名前 | 値 |
|---|---|---|
| Variable | `MIRROR_REPO` | `Fortyworks/stream-spook-obs-plugin` |
| Secret | `MIRROR_TOKEN` | ミラーに書ける fine-grained PAT（Repository access: ミラーだけ、Permissions: Contents = Read and write）。Release を作るのもこの token なので、作った人として出るのはその持ち主 |
| Variable（任意） | `MIRROR_COMMIT_NAME` / `MIRROR_COMMIT_EMAIL` | スナップショットのコミットの作者。省略時は the40san |

`MIRROR_REPO` が無ければジョブごと動かない（fork や手元の検証で勝手に push しない）。ミラー側には何も置かなくてよい（空のリポジトリで始められる）。zip の `README.txt` と本体の「OSS ライセンス」画面が指すのもミラーの URL。
