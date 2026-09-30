// 文言が ja / en の両方にあるかの見張り。
//
// OBS は無い文言キーをそのまま画面に出す（"Film.Grain" のように）。
//   - C のソースが obs_module_text() で引いているキー
//   - data/locale/en-US.ini / ja-JP.ini に書いてあるキー
// を突き合わせる。使い方: node tools/check-locale.mjs（終了コード 0 で OK）

import { readFileSync, readdirSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const srcDir = join(root, "src");
const localeDir = join(root, "data", "locale");

function iniKeys(file) {
  const out = new Map();
  for (const raw of readFileSync(join(localeDir, file), "utf8").split(/\r?\n/)) {
    const line = raw.trim();
    if (!line || line.startsWith(";") || line.startsWith("#")) continue;
    const m = /^([^=\s]+)="(.*)"$/.exec(line);
    if (!m) throw new Error(`${file}: 読めない行 → ${line}`);
    out.set(m[1], m[2]);
  }
  return out;
}

function usedKeys() {
  const keys = new Set();
  for (const name of readdirSync(srcDir)) {
    if (!/\.(c|h)$/.test(name)) continue;
    const src = readFileSync(join(srcDir, name), "utf8");
    for (const m of src.matchAll(/obs_module_text\("([^"]+)"\)/g)) keys.add(m[1]);
    // fx_anim_properties(props, "<group text key>", ...) は引数で文言キーを受ける
    for (const m of src.matchAll(/fx_anim_properties\([^,]+,\s*"([^"]+)"/g)) keys.add(m[1]);
  }
  return keys;
}

function filterIds() {
  const ids = [];
  for (const name of readdirSync(srcDir)) {
    if (!name.endsWith(".c")) continue;
    const src = readFileSync(join(srcDir, name), "utf8");
    for (const m of src.matchAll(/\.id\s*=\s*"([^"]+)"/g)) ids.push(m[1]);
  }
  return ids;
}

const problems = [];
const en = iniKeys("en-US.ini");
const ja = iniKeys("ja-JP.ini");
const used = usedKeys();

for (const k of used) {
  if (!en.has(k)) problems.push(`en-US.ini に無い: ${k}`);
  if (!ja.has(k)) problems.push(`ja-JP.ini に無い: ${k}`);
}
for (const k of en.keys()) if (!ja.has(k)) problems.push(`ja-JP.ini だけに無い: ${k}`);
for (const k of ja.keys()) if (!en.has(k)) problems.push(`en-US.ini だけに無い: ${k}`);
for (const k of en.keys()) if (!used.has(k)) problems.push(`使っていないキー: ${k}`);
for (const [file, map] of [["en-US.ini", en], ["ja-JP.ini", ja]]) {
  for (const [k, v] of map) {
    if (v === "") problems.push(`${file}: 空の文言 ${k}`);
    // フィルタ名は「StreamSpook: 」で始める（OBS のフィルタ一覧で見つけやすくするため）
    if (k.endsWith(".Name") && !v.startsWith("StreamSpook: ")) problems.push(`${file}: ${k} が StreamSpook: で始まっていない`);
  }
}
// フィルタ ID は stream_spook_ で始める（obs-websocket の GetSourceFilterKindList で見分けるため）
const ids = filterIds();
if (ids.length === 0) problems.push("ソースの種類 ID が 1 つも見つからない");
for (const id of ids) if (!/^stream_spook_[a-z_]+$/.test(id)) problems.push(`フィルタ ID の付け方が違う: ${id}`);

if (problems.length > 0) {
  console.error(problems.map((p) => `[check-locale] ${p}`).join("\n"));
  process.exit(1);
}
console.log(`[check-locale] OK（キー ${en.size} 件、種類 ${ids.length} 件）`);
