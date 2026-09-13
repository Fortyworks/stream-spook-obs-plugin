# StreamSpook for OBS

[StreamSpook](https://streamspook.app) が OBS の中で受け持つぶん ―― 配信画面そのものに掛けるポストエフェクトなど、ブラウザソースのオーバーレイでは届かないもの ―― のネイティブプラグイン。モジュール名は `stream-spook`、土台は [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate)。

- **ライセンス: GPL-2.0-or-later**（[LICENSE](./LICENSE)）。libobs が GPL-2.0-or-later で、そのヘッダのコード（マクロ・`static inline`）がこの DLL に直接含まれるため。StreamSpook 本体とは別のプログラムで、話すのは obs-websocket の JSON だけ
- **公開しているのは [Fortyworks/stream-spook-obs-plugin](https://github.com/Fortyworks/stream-spook-obs-plugin)。** リリースごとのスナップショット（1 版 = 1 コミット）で、開発の履歴と PR は入っていない。開発は非公開のリポジトリで行い、タグを打つと Actions がミラーへ積む（「5. 公開リポジトリ（ミラー）」）
- **いま入っているもの:** ポストエフェクト 8 種（古い映画 / 黒澤モード / モザイク / ビネット / グリッチ / 色収差 / カラーグレーディング / レンズのゆがみ）
- **配り方:** Release の zip を StreamSpook 本体が同梱し、アプリの「OBS プラグイン」ページから OBS のユーザー用プラグインフォルダへ入れる。手で入れることもできる（下）

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

管理者権限の要らないユーザー用の置き場所へ:

```powershell
cmake --install build_x64 --config RelWithDebInfo --prefix "$env:APPDATA\obs-studio\plugins"
```

置かれるのは `%APPDATA%\obs-studio\plugins\stream-spook\bin\64bit\stream-spook.dll` と `…\stream-spook\data\`。**OBS を再起動**すると、フィルタ一覧に「StreamSpook: …」が 8 つ並ぶ。外すときはこのフォルダごと消す。

- OBS が起動中だと DLL の上書きに失敗する（初回の新規コピーは通る）
- ポータブルモードの OBS は `%APPDATA%` を見ないので、この置き場所では読まれない

### OBS を起動せずに確かめる（smoke）

```powershell
pwsh tools/smoke/Run-Smoke.ps1
```

`.deps` の OBS ソースから `libobs-d3d11` を組み、プラグインを libobs に読み込んで全フィルタを作り（＝ `.effect` を実際にコンパイルし）、2 色のソースに掛けてピクセルを読み戻す（座標をずらすものは境目の色が動くこと、樽型のゆがみは角が透明になることまで見る）。終了コード 0 で OK。シェーダーのエラーは `[obs 300]` 以下の行に出る。`.effect` を触ったら必ず回す。

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

## 3. つくり

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
  chromatic-filter.c / grade-filter.c / lens-filter.c
data/
  effects/*.effect    描き方そのもの（HLSL 風の OBS effect）
  locale/{en-US,ja-JP}.ini
tools/
  package.mjs         配布物（release/ と zip）を作る
  check-locale.mjs    文言の突き合わせ
  smoke/              OBS を起動せずに読み込んで確かめる道具
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
- **秘密にしたいものを置かない。** このリポジトリは GPL で全部公開される。独自のロジックは StreamSpook 本体に置き、ここは「OBS の中でしかできないこと」を薄く受け持つ。本体のコードをここへ写さない（GPL になる）し、ここのコードを本体へ写さない（本体が GPL の派生物になる）

## 4. リリース

1. `buildspec.json` の `version` を上げてコミット
2. 同じ番号のタグを打つ: `git tag v0.2.0 && git push origin v0.2.0`
3. GitHub Actions がビルドして Release を作る（`stream-spook-<版>-windows-x64.zip` と `.sha256`）
4. StreamSpook 本体の `src-tauri/obs-plugin.json` に版と sha256 を書く。本体のリリースがその zip を取って同梱する

`manifest.json`（`{ "version": "…" }`）と `stream-spook/` の並びは本体の `src-tauri/src/obs_plugin.rs` が読む契約なので、形を変えるときは両方を直す。

## 5. 公開リポジトリ（ミラー）

GPL の「ソースを渡す」義務は、公開用のミラー [Fortyworks/stream-spook-obs-plugin](https://github.com/Fortyworks/stream-spook-obs-plugin) で満たす。こちら（開発用）は非公開のままで、履歴・PR・Actions の実行はここにしか残らない。

タグ `v*` を打つと `build.yml` の `mirror` ジョブが:

1. そのタグのツリーを `git archive` で取り出し（`.gitattributes` の `export-ignore` でワークフローは外す）、ミラーの `main` に **1 コミット**（メッセージは版の番号）として積んで、同じ番号のタグを打つ。作者は 1 つのアカウント（既定は the40san。メールは GitHub の noreply アドレス）にそろえるので、ミラーの履歴に他の名前は出ない。変えるならリポジトリ変数 `MIRROR_COMMIT_NAME` / `MIRROR_COMMIT_EMAIL`
2. こちらの Release と**同じ zip / .sha256 をそのまま**ミラーの Release に置く（作り直さない。本体が固定している sha256 が、どちらから取っても一致するように）

要るもの（このリポジトリの Settings）:

| 種類 | 名前 | 値 |
|---|---|---|
| Variable | `MIRROR_REPO` | `Fortyworks/stream-spook-obs-plugin` |
| Secret | `MIRROR_TOKEN` | ミラーに書ける fine-grained PAT（Repository access: ミラーだけ、Permissions: Contents = Read and write）。Release を作るのもこの token なので、作った人として出るのはその持ち主 |
| Variable（任意） | `MIRROR_COMMIT_NAME` / `MIRROR_COMMIT_EMAIL` | スナップショットのコミットの作者。省略時は the40san |

`MIRROR_REPO` が無ければジョブごと動かない（fork や手元の検証で勝手に push しない）。ミラー側には何も置かなくてよい（空のリポジトリで始められる）。zip の `README.txt` と本体の「OSS ライセンス」画面が指すのもミラーの URL。
