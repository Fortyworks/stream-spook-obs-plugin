// 配布物を作る。
//
// build_x64 の出来上がりを cmake --install で release/ に置き、そのまま OBS が
// Windows で探すプラグインフォルダ（%ProgramData%\obs-studio\plugins）に入れられる形に
// そろえる。StreamSpook 本体はこの zip を Release から取って同梱する
// （manifest.json の形は本体側の src-tauri/src/obs_plugin.rs と対）。
//
//   release/
//     manifest.json                    { "version": "<buildspec.json の version>" }
//     stream-spook/
//       LICENSE                        GPL-2.0-or-later（DLL と一緒に運ぶ）
//       README.txt                     何であるか・ソースの在りか
//       bin/64bit/stream-spook.dll
//       data/effects/*.effect, data/locale/*.ini
//   stream-spook-<version>-windows-x64.zip        release/ の中身
//   stream-spook-<version>-windows-x64.zip.sha256
//
// 使い方: node tools/package.mjs（先に cmake --build --preset windows-x64 を済ませておく）

import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, readdirSync, rmSync, statSync, unlinkSync, writeFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const buildDir = join(root, "build_x64");
const releaseDir = join(root, "release");
const spec = JSON.parse(readFileSync(join(root, "buildspec.json"), "utf8"));
const version = spec.version;
const name = spec.name; // stream-spook

/** PATH の cmake か、無ければ Visual Studio 同梱の cmake */
function findCmake() {
  if (spawnSync("cmake", ["--version"], { stdio: "ignore" }).status === 0) return "cmake";
  const vswhere = join(
    process.env["ProgramFiles(x86)"] ?? "C:\\Program Files (x86)",
    "Microsoft Visual Studio",
    "Installer",
    "vswhere.exe",
  );
  if (existsSync(vswhere)) {
    const vs = spawnSync(
      vswhere,
      ["-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
      { encoding: "utf8" },
    ).stdout.trim();
    const bundled = join(vs, "Common7", "IDE", "CommonExtensions", "Microsoft", "CMake", "CMake", "bin", "cmake.exe");
    if (vs && existsSync(bundled)) return bundled;
  }
  console.error("[package] cmake が見つかりません。Visual Studio 2022（C++ ツール）を入れるか、cmake を PATH に通してください");
  process.exit(1);
}

function run(cmd, args) {
  const r = spawnSync(cmd, args, { cwd: root, stdio: "inherit" });
  if (r.status !== 0) {
    console.error(`[package] ${cmd} ${args.join(" ")} が失敗しました（終了コード ${r.status}）`);
    process.exit(r.status ?? 1);
  }
}

function walk(dir, out = []) {
  for (const n of readdirSync(dir)) {
    const p = join(dir, n);
    if (statSync(p).isDirectory()) walk(p, out);
    else out.push(p);
  }
  return out;
}

if (!existsSync(buildDir)) {
  console.error("[package] build_x64 がありません。先に cmake --preset windows-x64 / --build --preset windows-x64 を回してください");
  process.exit(1);
}

rmSync(releaseDir, { recursive: true, force: true });
mkdirSync(releaseDir, { recursive: true });
run(findCmake(), ["--install", buildDir, "--config", "RelWithDebInfo", "--prefix", releaseDir]);

// PDB は配布物に要らない
for (const f of walk(releaseDir)) if (f.toLowerCase().endsWith(".pdb")) unlinkSync(f);

const pluginDir = join(releaseDir, name);
const dll = join(pluginDir, "bin", "64bit", `${name}.dll`);
if (!existsSync(dll)) {
  console.error(`[package] DLL がありません: ${dll}`);
  process.exit(1);
}

// ライセンスと出どころは DLL と同じフォルダで運ぶ（OBS は知らないファイルを無視する）
writeFileSync(join(pluginDir, "LICENSE"), readFileSync(join(root, "LICENSE")));
writeFileSync(
  join(pluginDir, "README.txt"),
  [
    `${spec.displayName} ${version}`,
    "",
    "OBS Studio plugin used by StreamSpook (post effects etc.).",
    "Licensed under the GNU General Public License v2.0 or later; see LICENSE.",
    "Source code: https://github.com/Fortyworks/stream-spook-obs-plugin",
    "",
    "Install: put this folder at %ProgramData%\\obs-studio\\plugins\\stream-spook and restart OBS.",
    "",
  ].join("\r\n"),
);
writeFileSync(join(releaseDir, "manifest.json"), JSON.stringify({ version }, null, 2) + "\n");

// zip。Windows 10 以降に入っている bsdtar（System32 の tar.exe）は -a で拡張子から zip を選ぶ。
// PATH 上の tar は Git Bash の GNU tar のことがあり、それは zip を書けないので場所で指す
const zipName = `${name}-${version}-windows-x64.zip`;
const zipPath = join(root, zipName);
rmSync(zipPath, { force: true });
const tar =
  process.platform === "win32" ? join(process.env.SystemRoot ?? "C:\\Windows", "System32", "tar.exe") : "tar";
run(tar, ["-a", "-cf", zipPath, "-C", releaseDir, "manifest.json", name]);

const sha256 = createHash("sha256").update(readFileSync(zipPath)).digest("hex");
writeFileSync(`${zipPath}.sha256`, `${sha256}  ${zipName}\n`);

console.log(`[package] ${zipName}`);
console.log(`[package] sha256 ${sha256}`);
for (const f of walk(releaseDir)) console.log(`[package]   ${f.slice(releaseDir.length + 1)}`);
