//! Writing and removing the patch.

use crate::engines::Platform;
use crate::plan::{Plan, ENGINE_SUBDIR};
use crate::json::{str_array, Json};
use crate::util;
use std::path::{Component, Path};

pub const MANIFEST: &str = ".mkxpz-patcher";
const BACKUP_SUFFIX: &str = ".pre-mkxpz";

#[derive(Clone, Debug)]
pub struct Manifest {
    pub platforms: Vec<String>,
    pub files: Vec<String>,
    /// original name -> backup name
    pub backups: Vec<(String, String)>,
}

fn safe_rel(rel: &str, manifest: &Path) -> Result<String, String> {
    let p = Path::new(rel);
    let ok = !rel.is_empty() && p.components().all(|c| matches!(c, Component::Normal(_)));
    if ok { Ok(rel.to_string()) } else { Err(format!("Refusing to use suspicious path {:?} in {}", rel, manifest.display())) }
}

pub fn read_manifest(game_dir: &Path) -> Result<Option<Manifest>, String> {
    let path = game_dir.join(MANIFEST);
    if !path.is_file() {
        return Ok(None);
    }
    let damaged = || format!("{} is damaged; remove it by hand if this game isn't patched.", path.display());
    let v = Json::parse(&std::fs::read_to_string(&path).map_err(|_| damaged())?).ok_or_else(damaged)?;
    let strs = |key: &str| -> Vec<String> {
        v.get(key).and_then(|x| x.as_array()).map(|a| a.iter().filter_map(|s| s.as_str().map(String::from)).collect())
            .unwrap_or_default()
    };
    let files = strs("files").iter().map(|f| safe_rel(f, &path)).collect::<Result<Vec<_>, _>>()?;
    let mut platforms = strs("platforms");
    if platforms.is_empty() {
        platforms.push("linux".into()); // version 1 manifests were Linux-only
    }
    let mut backups = Vec::new();
    if let Some(obj) = v.get("backups").and_then(|b| b.as_object()) {
        for (k, b) in obj {
            if let Some(b) = b.as_str() {
                backups.push((safe_rel(k, &path)?, safe_rel(b, &path)?));
            }
        }
    }
    Ok(Some(Manifest { platforms, files, backups }))
}

fn write_manifest(game_dir: &Path, m: &Manifest) -> Result<(), String> {
    let strs = |v: &[String]| Json::Arr(v.iter().map(|s| Json::Str(s.clone())).collect());
    let v = Json::Obj(vec![
        ("version".into(), Json::Num(3.0)),
        ("platforms".into(), strs(&m.platforms)),
        ("files".into(), strs(&m.files)),
        ("backups".into(), Json::Obj(m.backups.iter().map(|(k, b)| (k.clone(), Json::Str(b.clone()))).collect())),
    ]);
    std::fs::write(game_dir.join(MANIFEST), v.pretty() + "\n")
        .map_err(|e| format!("couldn't write {}: {}", MANIFEST, e))
}

/// Folder for screen shaders inside ENGINE_SUBDIR; the player's files in it
/// survive removing or redoing the patch.
const SHADERS_DIR: &str = "shaders";
const SHADERS_README_FILE: &str = "README.txt";
const SHADERS_README: &str = "Screen shaders for mkxp-z (added by the MKXP-Z Patcher)

Put shaders into this folder (subfolders are fine), then choose one in the game
with F1 > Screen shader. The choice is saved for this game.

Format: libretro GLSL shaders, i.e. presets (.glslp) and their passes (.glsl).
The whole collection is at https://github.com/libretro/glsl-shaders - download
it (Code > Download ZIP) and unpack it here; every .glslp then shows up in the
list (e.g. crt/crt-geom.glslp, scalefx/scalefx.glslp, xbrz/xbrz-freescale.glslp).

Not supported: the Cg versions (.cg / .cgp, libretro/common-shaders) - there's
no Cg compiler anymore; use their GLSL counterparts from glsl-shaders instead -
and slang shaders (.slang / .slangp, libretro/slang-shaders).

Not every preset runs everywhere: some rely on desktop-only GLSL that the
Windows build's graphics layer rejects, and under Wine / Proton some heavy ones
(e.g. crt-royale, xbrz, gtu) fail in Wine's Direct3D shader compiler. The game
then says so and continues without the shader. Heavy multi-pass presets can
also be slow on weak graphics chips.

This folder is kept when the patch is removed or applied again.
";

/// Whether the shader folder holds anything besides our README.
fn has_user_shaders(dir: &Path) -> bool {
    std::fs::read_dir(dir).map_or(false, |rd| rd.flatten().any(|e| e.file_name() != SHADERS_README_FILE))
}

/// mkxp-z's own files of the player in ENGINE_SUBDIR (its "dataPath"): the
/// settings from the F1 menu and the key bindings.
pub fn is_player_file(name: &str) -> bool {
    name == "mkxp.json" || name.starts_with("keybindings.mkxp")
}

/// What of ENGINE_SUBDIR belongs to the player and stays when the patch is
/// removed or applied again: their shaders, settings and key bindings.
fn is_kept(dir: &Path, name: &str) -> bool {
    (name == SHADERS_DIR && has_user_shaders(&dir.join(SHADERS_DIR))) || is_player_file(name)
}

/// Remove ENGINE_SUBDIR, but keep the player's files in it (see is_kept).
fn remove_engine_dir(dir: &Path, log: &dyn Fn(String)) -> Result<(), String> {
    let mut kept = Vec::new();
    for e in std::fs::read_dir(dir).map_err(|e| e.to_string())?.flatten() {
        let name = e.file_name().to_string_lossy().into_owned();
        if is_kept(dir, &name) {
            kept.push(name);
        } else {
            util::remove_path(&e.path()).map_err(|err| format!("couldn't remove {}: {}", e.path().display(), err))?;
        }
    }
    if kept.is_empty() {
        return util::remove_path(dir).map_err(|e| format!("couldn't remove {}: {}", dir.display(), e));
    }
    kept.sort();
    log(format!("Kept your files in {}: {}", ENGINE_SUBDIR, kept.join(", ")));
    Ok(())
}

pub fn unpatch(game_dir: &Path, log: &dyn Fn(String)) -> Result<(), String> {
    let m = read_manifest(game_dir)?
        .ok_or_else(|| format!("This folder wasn't patched by the patcher (no {} found).", MANIFEST))?;
    for rel in &m.files {
        let p = game_dir.join(rel);
        if rel == ENGINE_SUBDIR && p.is_dir() {
            log(format!("Removing {}", rel));
            remove_engine_dir(&p, log)?;
            continue;
        }
        if p.exists() || std::fs::symlink_metadata(&p).is_ok() {
            log(format!("Removing {}", rel));
            util::remove_path(&p).map_err(|e| format!("couldn't remove {}: {}", p.display(), e))?;
        }
    }
    for (orig, backup) in &m.backups {
        let b = game_dir.join(backup);
        if b.exists() {
            log(format!("Restoring {}", orig));
            std::fs::rename(&b, game_dir.join(orig)).map_err(|e| e.to_string())?;
        }
    }
    std::fs::remove_file(game_dir.join(MANIFEST)).map_err(|e| e.to_string())?;
    log("Patch removed.".into());
    Ok(())
}

// ------------------------------------------------------------------ contents

const LAUNCHER_LINUX: &str = r#"#!/usr/bin/env bash
# Starts this game natively with mkxp-z (added by the MKXP-Z Patcher).
# Extra arguments are passed through: "test" = debug mode, "btest" = battle test.
set -euo pipefail
GAME="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
SHARED="$GAME/__SUBDIR__"

case "$(uname -m)" in
    x86_64|amd64)        CHOICES="__CHOICES64__" ;;
    aarch64|arm64)       CHOICES="__CHOICESARM64__" ;;
    armv7*|armv8l|armhf) CHOICES="__CHOICESARMHF__" ;;
    *)                   CHOICES="__CHOICES32__" ;;
esac
ENGINE=""
for c in $CHOICES; do
    if [[ -x "$SHARED/$c" ]]; then ENGINE="$SHARED/$c"; break; fi
done
if [[ -z "$ENGINE" ]]; then
    echo "No mkxp-z engine for this computer ($(uname -m)) in $SHARED" >&2
    exit 1
fi

# MIDI music needs a SoundFont (.sf2; mkxp-z can't read .sf3). Put your own at
# __SUBDIR__/soundfont.sf2, or the first one installed here is linked at start.
if [[ ! -f "$SHARED/soundfont.sf2" ]]; then
    shopt -s nullglob
    for sf in /usr/share/soundfonts/FluidR3_GM.sf2 /usr/share/soundfonts/default.sf2 \
              /usr/share/sounds/sf2/FluidR3_GM.sf2 /usr/share/sounds/sf2/default-GM.sf2 \
              /usr/share/soundfonts/*.sf2 /usr/share/sounds/sf2/*.sf2 \
              "$HOME"/.local/share/soundfonts/*.sf2; do
        if [[ -f "$sf" ]]; then ln -sfn "$sf" "$SHARED/soundfont.sf2"; break; fi
    done
    shopt -u nullglob
fi

# mkxp-z reads mkxp.json from SRCDIR (the engine's folder); that config
# switches to the game folder ("gameFolder": "../..").
export SRCDIR="$(dirname "$ENGINE")"
cd "$GAME"
exec "$ENGINE" "$@"
"#;

/// The tiny Windows launcher (../launcher), embedded at build time; empty if it
/// wasn't available, then a .cmd file is written instead.
pub const LAUNCHER_EXE: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/launcher.exe"));
/// mkxp-z's icon, used when the game's own Game.exe has none.
pub const MKXPZ_ICON: &[u8] = include_bytes!("../assets/mkxpz.ico");

pub fn have_exe_launcher() -> bool {
    !LAUNCHER_EXE.is_empty()
}

/// The game's own icon, from Game.exe.
fn game_icon(plan: &Plan) -> Option<crate::pe::Icon> {
    let game_exe = plan.game_dir.join(format!("{}.exe", plan.exec_name));
    std::fs::read(&game_exe).ok().and_then(|d| crate::pe::icon_from_exe(&d))
}

/// The RTP inside ENGINE_SUBDIR when it's bundled (Plan::bundle_rtp).
const BUNDLED_RTP: &str = "RTP";

/// File name of the game's icon in ENGINE_SUBDIR, for mkxp-z's window.
const ICON_FILE: &str = "icon.png";

/// "Game (mkxp-z).exe" with the game's own icon (from Game.exe), or mkxp-z's.
fn windows_launcher_exe(plan: &Plan, log: &dyn Fn(String)) -> Vec<u8> {
    let icon = game_icon(plan);
    if icon.is_none() {
        log(format!("{}.exe has no icon to copy; using mkxp-z's.", plan.exec_name));
    }
    let icon = icon.or_else(|| crate::pe::icon_from_ico(MKXPZ_ICON));
    match icon.map(|i| crate::pe::with_icon(LAUNCHER_EXE, &i)) {
        Some(Ok(exe)) => exe,
        Some(Err(e)) => {
            log(format!("Couldn't add the icon to the launcher ({}); it gets the default one.", e));
            LAUNCHER_EXE.to_vec()
        }
        None => LAUNCHER_EXE.to_vec(),
    }
}

// Fallback when the launcher exe isn't embedded. mkxp-z on Windows reads
// mkxp.json next to its .exe; that config switches to the game folder.
const LAUNCHER_WINDOWS: &str = "@echo off\r\nrem Starts this game with mkxp-z (added by the MKXP-Z Patcher).\r\n\
                                start \"\" \"%~dp0__SUBDIR__\\__PLATDIR__\\__EXE__\" %*\r\n";

/// Steam launch options for Linux (Proton): swap the game's exe in Steam's
/// command for the mkxp-z launcher, so Proton runs that instead.
pub fn proton_launch_options(exec_name: &str) -> String {
    format!("bash -c 'exec \"${{@/%{0}.exe/{0} (mkxp-z).exe}}\"' -- %command%", exec_name)
}

const README: &str = "Added by the MKXP-Z Patcher: the mkxp-z engine for this game.

  Game:    {title}
  Engine:  mkxp-z ({rgss})

Start the game with:
{launchers}
Files added to the game folder: {added}
Run the patcher again and choose \"Remove patch\" to take them out again.
The game's original files are unchanged, so Game.exe still works as before.

Layout of this folder:
  linux/, linux32/, windows/   the engine for each platform, its Ruby standard
                               library and its mkxp.json
  scripts/      mkxp-z's compatibility scripts (CC0)
  compat/       patcher-made compatibility scripts, if the game needed them
  icon.png      the game's icon (from Game.exe), for the window
  shaders/      screen shaders you add (libretro GLSL, see shaders/README.txt);
                kept when the patch is removed or applied again
  rtp/Fonts/    WenQuanYi Micro Hei, if the game's own font lacks glyphs for its
                text (Apache-2.0 or GPLv3, http://wenq.org)
  soundfont.sf2 SoundFont for MIDI music (on Linux possibly a link to an
                installed one)

Settings (F1): key bindings, and the options \"Reduce screen flickering\" (vsync),
\"Smooth movement\" (frame interpolation), \"Start in fullscreen\" and the music
and sound levels. They're saved in this folder (mkxp-z/mkxp.json, next to the key
bindings in keybindings.mkxp*; AppImages: ~/.config/<game title>), and take
precedence over mkxp-z/<platform>/mkxp.json. They stay when the patch is removed
or applied again; settings saved by older versions in the user's data folder
(%APPDATA%\\<game title>, ~/.local/share/<game title>) are taken over once.

Screen shaders (CRT look, smoothing filters, ...): put libretro GLSL shaders
(https://github.com/libretro/glsl-shaders) into mkxp-z/shaders and choose one
with F1 > Screen shader.

Smooth movement (frame interpolation, off by default): choose a rate with the
slider in the F1 menu (mouse wheel: 1 FPS steps), from just above the game's
own rate (61 FPS; RPG Maker XP: 41) up to the refresh rate of the display the
game is on (the marks 2x, 3x ... show the multiples of the game's rate, where
the frames come out the most even), or \"Unlimited\" (as many
frames as the computer manages, without vsync); or set \"frameInterpolation\"
in <platform>/mkxp.json (0: off, a number: that rate, 1000: the display's rate,
1001: unlimited).
On displays faster than the game (e.g. 120/144 Hz; RPG Maker XP games also on
60 Hz), the frames between the game's own are then drawn with blended map,
sprite and picture positions; the game itself keeps its speed. While it's on,
the FPS display (F2) shows the game's rate and the rate sent to the display,
e.g. \"60/144 FPS\".

Steam versions: the 32-bit Windows engine uses the game's own steam_api.dll, so
start the game through Steam (started directly, Steam restarts Game.exe).
In Steam: right-click the game > Properties > Launch options:
  Windows:
    \"<game folder>\\mkxp-z\\windows\\mkxp-z.i686.exe\" %command%
  Linux (Proton; replaces Game.exe in Steam's command with the mkxp-z launcher):
    {PROTON}
The native Linux engine can't load the Windows Steam DLL, so Steam versions quit
at startup with it.
To check the Steam functions (e.g. that achievements reach Steam), create an
empty file mkxp-z/steam-log.txt: every call into steam_api.dll is then added to
it, with its result (delete the file to stop).

mkxp-z is licensed under the GPL (see LICENSE.txt in this folder).
Source code: https://github.com/mkxp-z/mkxp-z
";

fn json(s: &str) -> String {
    crate::json::quote(s)
}

/// mkxp.json for one engine; it sits next to the engine. Paths are relative to
/// the game folder, which the config switches to first.
pub fn build_config(plan: &Plan, p: Platform) -> String {
    let mut opts: Vec<(&str, String)> = vec![
        ("gameFolder", json("../..") /* mkxp-z/<platform>/ -> game folder */),
        ("rgssVersion", plan.rgss.to_string()),
        ("pathCache", "true".into()),          // Windows-style case-insensitive file lookup
        ("winResizable", "true".into()),
        ("fixedAspectRatio", "true".into()),
        ("enableReset", "true".into()),
        // bilinear filtering when the window is larger than the game's screen
        ("smoothScaling", "1".into()),
        // scale by whole multiples first (sharp pixels), then smoothly to fit
        ("integerScalingActive", "true".into()),
        // smooth movement on displays faster than the game (logic stays at its
        // rate): 0 off (default), else the rate to show frames at (1000: the
        // display's refresh rate, 1001: unlimited); listed so it's easy to find
        ("frameInterpolation", "0".into()),
        // mkxp-z's own files (settings from F1, key bindings) in the game's
        // mkxp-z folder instead of the user's data folder; AppImages set
        // MKXPZ_DATA_PATH (~/.config/<title>), which takes precedence
        ("dataPath", json(ENGINE_SUBDIR)),
    ];
    if plan.icon_png.is_some() {
        // the game's icon for the window (instead of mkxp-z's)
        opts.push(("iconPath", json(&format!("{}/{}", ENGINE_SUBDIR, ICON_FILE))));
    }
    if plan.exec_name != "Game" {
        opts.push(("execName", json(&plan.exec_name)));
    }
    if plan.engine(p).map_or(false, |e| e.has_stdlib()) {
        opts.push(("rubyLoadpath", str_array(&[format!("{}/{}/stdlib", ENGINE_SUBDIR, p.sub())])));
    }
    opts.push(("preloadScript", str_array(&plan.preload)));
    let mut rtps = Vec::new();
    // an RTP found on this computer is an absolute path: only valid for this OS
    if let Some(r) = &plan.rtp {
        if plan.bundle_rtp {
            rtps.push(format!("{}/{}", ENGINE_SUBDIR, BUNDLED_RTP));
        } else if p.is_windows() == cfg!(windows) {
            rtps.push(r.display().to_string());
        }
    }
    if plan.cjk_font.is_some() {
        rtps.push(format!("{}/rtp", ENGINE_SUBDIR)); // holds Fonts/wqymicrohei.ttf
        // mkxp-z lowercases the requested name before looking up substitutions,
        // so both sides must be lowercase or the entry never matches.
        if let Some(f) = &plan.fonts {
            let subs: Vec<String> = f.requested.iter()
                .map(|n| format!("{}>{}", n.to_lowercase(), crate::fonts::CJK_FONT_FAMILY)).collect();
            opts.push(("fontSub", str_array(&subs)));
        }
    }
    if !rtps.is_empty() {
        opts.push(("RTP", str_array(&rtps)));
    }
    opts.push(("midiSoundFont", json(&format!("{}/soundfont.sf2", ENGINE_SUBDIR))));
    let body: Vec<String> = opts.iter().map(|(k, v)| format!("    {}: {}", json(k), v)).collect();
    format!("{{\n    // Generated by the MKXP-Z Patcher ({}). Option reference:\n    \
             // https://github.com/mkxp-z/mkxp-z/blob/dev/mkxp.json\n{}\n}}\n", p.label(), body.join(",\n"))
}

fn linux_launcher(plan: &Plan) -> String {
    // the engines that run on each kind of CPU, best first
    let list = |ps: &[Platform]| ps.iter().filter(|p| plan.platforms.contains(p))
        .map(|p| format!("{}/{}", p.sub(), p.exe())).collect::<Vec<_>>().join(" ");
    LAUNCHER_LINUX.replace("__SUBDIR__", ENGINE_SUBDIR)
        .replace("__CHOICES64__", &list(&[Platform::Linux64, Platform::Linux32]))
        .replace("__CHOICES32__", &list(&[Platform::Linux32]))
        .replace("__CHOICESARM64__", &list(&[Platform::LinuxArm64, Platform::LinuxArmhf]))
        .replace("__CHOICESARMHF__", &list(&[Platform::LinuxArmhf]))
}

pub fn apply(plan: &mut Plan, log: &dyn Fn(String)) -> Result<(), String> {
    let g = plan.game_dir.clone();
    if !plan.ready() {
        return Err(format!("Engines not built yet: {}",
                           plan.missing_engines.iter().map(|p| p.label()).collect::<Vec<_>>().join(", ")));
    }
    let probe = g.join(".mkxpz-write-test");
    std::fs::write(&probe, b"").map_err(|_| format!("The game folder isn't writable: {}", g.display()))?;
    let _ = std::fs::remove_file(&probe);

    if plan.patched() {
        log("Removing previous patch ...".into());
        unpatch(&g, &|_| {})?;
        plan.manifest = None;
    }
    let shared = g.join(ENGINE_SUBDIR);
    // only the player's files may be there (kept from an earlier patch)
    let only_shaders = std::fs::read_dir(&shared).map_or(false, |rd| rd.flatten().all(|e| {
        let n = e.file_name().to_string_lossy().into_owned();
        n == SHADERS_DIR || is_player_file(&n)
    }));
    if shared.exists() && !only_shaders {
        return Err(format!("{} already exists and isn't from this patcher; not touching it.", shared.display()));
    }

    let launchers = plan.launchers();
    let root_files: Vec<String> = launchers.iter().map(|(_, n)| n.clone()).collect();
    let mut m = Manifest { platforms: plan.platforms.iter().map(|p| p.key().to_string()).collect(),
                           files: Vec::new(), backups: Vec::new() };
    for name in &root_files {
        let p = g.join(name);
        if p.exists() || std::fs::symlink_metadata(&p).is_ok() {
            let mut backup = format!("{}{}", name, BACKUP_SUFFIX);
            let mut n = 1;
            while g.join(&backup).exists() {
                backup = format!("{}{}.{}", name, BACKUP_SUFFIX, n);
                n += 1;
            }
            log(format!("Backing up existing {} -> {}", name, backup));
            std::fs::rename(&p, g.join(&backup)).map_err(|e| e.to_string())?;
            m.backups.push((name.clone(), backup));
        }
    }
    write_manifest(&g, &m)?; // written early, so a failed patch can still be removed cleanly

    let result = (|| -> Result<(), String> {
        let io = |e: std::io::Error| e.to_string();
        std::fs::create_dir_all(&shared).map_err(io)?;
        m.files.push(ENGINE_SUBDIR.into());
        m.files.extend(root_files.iter().cloned());
        write_manifest(&g, &m)?;

        std::fs::create_dir_all(shared.join(SHADERS_DIR)).map_err(io)?;
        std::fs::write(shared.join(SHADERS_DIR).join(SHADERS_README_FILE), SHADERS_README).map_err(io)?;
        let icon = game_icon(plan);
        plan.icon_png = icon.as_ref().and_then(|i| i.to_png());
        if let Some(png) = &plan.icon_png {
            std::fs::create_dir_all(&shared).map_err(io)?;
            std::fs::write(shared.join(ICON_FILE), png).map_err(io)?;
        }
        for &p in &plan.platforms {
            let e = plan.engine(p).ok_or("engine vanished")?;
            let dst = shared.join(p.sub());
            log(format!("Adding {} engine ...", p.label()));
            std::fs::create_dir_all(&dst).map_err(io)?;
            for (name, src) in &e.files {
                util::copy_tree(src, &dst.join(name)).map_err(|err| format!("{}: {}", src.display(), err))?;
            }
            if p.is_windows() {
                if let Some(i) = &icon {
                    // the engine's own icon (Explorer, taskbar, its other windows)
                    let exe = dst.join(p.exe());
                    let res = std::fs::read(&exe).map_err(|e| e.to_string())
                        .and_then(|d| crate::pe::with_icon(&d, i))
                        .and_then(|d| std::fs::write(&exe, d).map_err(|e| e.to_string()));
                    if let Err(e) = res {
                        log(format!("Couldn't give the {} engine the game's icon ({}); it keeps mkxp-z's.", p.label(), e));
                    }
                }
            }
            util::make_executable(&dst.join(p.exe())).map_err(io)?;
            std::fs::write(dst.join("mkxp.json"), build_config(plan, p)).map_err(io)?;
        }
        if let Some(s) = &plan.scripts_src {
            util::copy_tree(s, &shared.join("scripts")).map_err(io)?;
        }
        if !plan.compat.is_empty() {
            std::fs::create_dir_all(shared.join("compat")).map_err(io)?;
            for name in &plan.compat {
                let text = crate::plan::compat_script(name).ok_or_else(|| format!("unknown compat script {}", name))?;
                std::fs::write(shared.join("compat").join(name), text).map_err(io)?;
            }
        }
        if let Some(l) = &plan.license_src {
            std::fs::copy(l, shared.join("LICENSE.txt")).map_err(io)?;
        }
        if let (Some(r), true) = (&plan.rtp, plan.bundle_rtp) {
            log(format!("Adding the RTP from {} ...", r.display()));
            util::copy_tree(r, &shared.join(BUNDLED_RTP)).map_err(io)?;
        }
        if let Some(f) = &plan.cjk_font {
            log("Adding CJK font (WenQuanYi Micro Hei) ...".into());
            std::fs::create_dir_all(shared.join("rtp/Fonts")).map_err(io)?;
            std::fs::copy(f, shared.join("rtp/Fonts").join(crate::fonts::CJK_FONT_FILE)).map_err(io)?;
        }
        if let Some(sf) = &plan.soundfont {
            log(format!("Adding SoundFont {} ...", sf.file_name().unwrap_or_default().to_string_lossy()));
            std::fs::copy(sf, shared.join("soundfont.sf2")).map_err(io)?;
        }
        let mut lines = Vec::new();
        for (p, name) in &launchers {
            log(format!("Writing launcher {} ...", name));
            if p.is_windows() {
                let e = plan.engine(*p).ok_or("engine vanished")?;
                if have_exe_launcher() {
                    std::fs::write(g.join(name), windows_launcher_exe(plan, log)).map_err(io)?;
                } else {
                    let text = LAUNCHER_WINDOWS.replace("__SUBDIR__", ENGINE_SUBDIR)
                        .replace("__PLATDIR__", p.sub()).replace("__EXE__", e.platform.exe());
                    std::fs::write(g.join(name), text).map_err(io)?;
                }
                lines.push(format!("  Windows:  {}", name));
            } else {
                std::fs::write(g.join(name), linux_launcher(plan)).map_err(io)?;
                util::make_executable(&g.join(name)).map_err(io)?;
                lines.push(format!("  Linux:    ./{}", name));
            }
        }
        let mut added = root_files.clone();
        added.push(format!("{}/", ENGINE_SUBDIR));
        added.push(MANIFEST.into());
        let readme = README.replace("{title}", &plan.title)
            .replace("{rgss}", crate::game::RGSS_NAMES[plan.rgss])
            .replace("{launchers}", &(lines.join("\n") + "\n"))
            .replace("{added}", &added.join(", "))
            .replace("{PROTON}", &proton_launch_options(&plan.exec_name));
        std::fs::write(shared.join("README.txt"), readme).map_err(io)?;
        Ok(())
    })();

    if let Err(e) = result {
        log("Failed - rolling back ...".into());
        let _ = write_manifest(&g, &m);
        let _ = unpatch(&g, &|_| {});
        return Err(e);
    }
    plan.manifest = Some(m);
    log(format!("Done. Start the game with: {}", root_files.join(", ")));
    Ok(())
}

#[cfg(test)]
mod shader_dir_tests {
    use super::*;

    #[test]
    fn keeps_player_settings() {
        let base = std::env::temp_dir().join(format!("mkxpz-settings-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&base);
        let dir = base.join("mkxp-z");
        std::fs::create_dir_all(dir.join("linux")).unwrap();
        std::fs::create_dir_all(dir.join("shaders")).unwrap();
        std::fs::write(dir.join("shaders").join(SHADERS_README_FILE), "x").unwrap();
        std::fs::write(dir.join("mkxp.json"), "{}").unwrap();
        std::fs::write(dir.join("keybindings.mkxp3"), "k").unwrap();
        std::fs::write(dir.join("README.txt"), "r").unwrap();
        remove_engine_dir(&dir, &|_| {}).unwrap();
        assert!(dir.join("mkxp.json").is_file() && dir.join("keybindings.mkxp3").is_file());
        assert!(!dir.join("linux").exists() && !dir.join("shaders").exists() && !dir.join("README.txt").exists());
        let _ = std::fs::remove_dir_all(&base);
    }

    #[test]
    fn keeps_player_shaders() {
        let base = std::env::temp_dir().join(format!("mkxpz-shaders-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&base);
        let dir = base.join("mkxp-z");
        std::fs::create_dir_all(dir.join("windows")).unwrap();
        std::fs::create_dir_all(dir.join("shaders")).unwrap();
        std::fs::write(dir.join("shaders").join(SHADERS_README_FILE), "x").unwrap();
        // only our README: everything goes
        remove_engine_dir(&dir, &|_| {}).unwrap();
        assert!(!dir.exists());
        // with a shader of the player's: only that stays
        std::fs::create_dir_all(dir.join("windows")).unwrap();
        std::fs::create_dir_all(dir.join("shaders/crt")).unwrap();
        std::fs::write(dir.join("shaders/crt/crt-geom.glslp"), "shaders = 1").unwrap();
        std::fs::write(dir.join("README.txt"), "r").unwrap();
        remove_engine_dir(&dir, &|_| {}).unwrap();
        assert!(dir.join("shaders/crt/crt-geom.glslp").is_file());
        assert!(!dir.join("windows").exists() && !dir.join("README.txt").exists());
        let _ = std::fs::remove_dir_all(&base);
    }
}
