//! Pack a game with its Linux engine into one AppImage file per CPU type.
//!
//! The AppImage holds a patched copy of the game (only the Linux engine for
//! that CPU, the RTP bundled if the game needs one) and starts it through
//! AppRun. An AppImage is read-only, but RPG Maker games write their saves and
//! settings (Game.ini, own text files) into the game folder, so AppRun runs the
//! game from a folder in the user's data folder that links to the files in the
//! AppImage; whatever the game writes ends up there, as real files.
//!
//! mkxp-z's own files (settings, key bindings) go to ~/.config/<title>.
//!
//! Needs mksquashfs (squashfs-tools) and the AppImage runtime, which is
//! downloaded from the AppImage project into the engines folder for each
//! build and removed again afterwards.

use crate::engines::{Paths, Platform};
use crate::plan::{Options, Plan};
use crate::util;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

/// The AppImage runtime (type 2, static): the program at the start of every
/// AppImage that mounts the image appended to it and starts AppRun.
const RUNTIME_URL: &str = "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-";

/// CPU name as AppImages (and their runtimes) use it.
pub fn arch(p: Platform) -> Option<&'static str> {
    match p.base() {
        Platform::Linux64 => Some("x86_64"),
        Platform::Linux32 => Some("i686"),
        Platform::LinuxArm64 => Some("aarch64"),
        Platform::LinuxArmhf => Some("armhf"),
        _ => None,
    }
}

/// Folder and file names from a game title: letters, digits, " -_." only.
pub(crate) fn safe_name(title: &str) -> String {
    let s: String = title.chars()
        .map(|c| if c.is_alphanumeric() || " -_.".contains(c) { c } else { '_' })
        .collect();
    let s = s.trim().trim_matches('.').to_string();
    if s.is_empty() { "Game".into() } else { s }
}

/// Where the AppImage for `p` goes: next to the game folder.
pub fn output_path(game_dir: &Path, title: &str, p: Platform) -> PathBuf {
    let parent = game_dir.parent().map(Path::to_path_buf).unwrap_or_else(|| PathBuf::from("."));
    parent.join(format!("{}-{}.AppImage", safe_name(title).replace(' ', "_"), arch(p).unwrap_or("unknown")))
}

/// Can AppImages be made here? (Linux, mksquashfs installed)
pub fn check_tools() -> Result<(), String> {
    if !cfg!(target_os = "linux") {
        return Err("AppImages can only be made on Linux.".into());
    }
    if util::which("mksquashfs").is_none() {
        return Err(format!("Making an AppImage needs mksquashfs. Install it with: {}", match util::distro() {
            util::Distro::Debian => "sudo apt install squashfs-tools",
            util::Distro::Arch => "sudo pacman -S squashfs-tools",
            _ => "sudo dnf install squashfs-tools",
        }));
    }
    Ok(())
}

/// The runtime for this CPU, downloaded into the engines folder
/// (<engines>/runtime-<arch>) and removed again once it's read.
fn runtime(paths: &Paths, arch: &str, log: &(dyn Fn(String) + Sync)) -> Result<Vec<u8>, String> {
    let dir = paths.resolved().engines;
    std::fs::create_dir_all(&dir).map_err(|e| format!("{}: {}", dir.display(), e))?;
    let file = dir.join(format!("runtime-{}", arch));
    let url = format!("{}{}", RUNTIME_URL, arch);
    log(format!("Downloading the AppImage runtime ({}) into {} ...", url, dir.display()));
    let ok = Command::new("curl").args(["-fsSL", "--retry", "3", "-o"]).arg(&file).arg(&url)
        .stdin(Stdio::null()).status().map_or(false, |s| s.success());
    let data = std::fs::read(&file).unwrap_or_default();
    let _ = std::fs::remove_file(&file);
    if !ok || !data.starts_with(b"\x7fELF") {
        return Err(format!("couldn't download the AppImage runtime from {}", url));
    }
    Ok(data)
}

/// AppRun: refreshes the writable game folder (links into the mounted
/// AppImage, whose mount point changes with every start) and starts Game.sh.
const APPRUN: &str = r#"#!/bin/sh
# Starts the game in this AppImage (made by the MKXP-Z Patcher).
# The AppImage is read-only, but the game writes saves and settings into its
# own folder. So it runs from __DATADIR__/game in your data folder
# (~/.local/share): links to the game's files in here, plus whatever the game
# writes, as real files. Delete that folder to start over (saves included).
set -e
HERE="$(dirname "$(readlink -f "$0")")"
GAME="${XDG_DATA_HOME:-$HOME/.local/share}/__DATADIR__/game"
# mkxp-z's own files (settings from F1, key bindings)
export MKXPZ_DATA_PATH="${XDG_CONFIG_HOME:-$HOME/.config}/__DATADIR__"
mkdir -p "$GAME"
# links of the last start point into its mount folder, which is gone now
find "$GAME" -type l -exec rm -f {} +
# a link for every file of the game; files there already (the game's own) stay
cp -rsn "$HERE/game/." "$GAME/" 2>/dev/null || true
# files games change themselves become real copies (made once, then kept)
find "$GAME" -type l \( -iname '*.ini' -o -iname '*.txt' -o -iname '*.cfg' \
    -o -iname 'save*.rxdata' -o -iname 'save*.rvdata' -o -iname 'save*.rvdata2' \) |
while IFS= read -r f; do
    t="$(readlink "$f")"
    rm -f "$f"
    cp "$t" "$f"
done
# the launcher finds the game next to itself: a real file, always this version
rm -f "$GAME/Game.sh"
cp "$HERE/game/Game.sh" "$GAME/Game.sh"
chmod +x "$GAME/Game.sh"
exec "$GAME/Game.sh" "$@"
"#;

fn desktop_file(title: &str, icon: &str) -> String {
    format!("[Desktop Entry]\nType=Application\nName={}\nExec=AppRun\nIcon={}\nCategories=Game;\nTerminal=false\n",
            title.replace('\n', " "), icon)
}

/// mksquashfs with zstd if it has it (faster to start), else gzip; both are
/// read by the AppImage runtime.
fn squash(dir: &Path, out: &Path, log: &(dyn Fn(String) + Sync)) -> Result<(), String> {
    let help = Command::new("mksquashfs").arg("-help").stdin(Stdio::null()).output()
        .map(|o| format!("{}{}", String::from_utf8_lossy(&o.stdout), String::from_utf8_lossy(&o.stderr)))
        .unwrap_or_default();
    let comp = if help.contains("zstd") { "zstd" } else { "gzip" };
    log(format!("Packing ({} compression) ...", comp));
    let st = Command::new("mksquashfs").arg(dir).arg(out)
        .args(["-root-owned", "-noappend", "-no-progress", "-quiet", "-comp", comp])
        .stdin(Stdio::null()).status().map_err(|e| format!("couldn't start mksquashfs: {}", e))?;
    if st.success() { Ok(()) } else { Err(format!("mksquashfs failed ({})", st)) }
}

/// Save files of RPG Maker games (Save1.rxdata, Save01.rvdata2 ...).
fn is_save(name: &str) -> bool {
    let n = name.to_ascii_lowercase();
    n.starts_with("save") && [".rxdata", ".rvdata", ".rvdata2"].iter().any(|e| n.ends_with(e))
}

/// Copy the game for the AppImage, without the player's saves (an AppImage
/// may be passed on), links (e.g. to system files) and AppImages made earlier.
/// Returns how many were left out.
pub(crate) fn copy_game(src: &Path, dst: &Path) -> std::io::Result<usize> {
    std::fs::create_dir_all(dst)?;
    let mut skipped = 0;
    for e in std::fs::read_dir(src)? {
        let e = e?;
        let name = e.file_name().to_string_lossy().into_owned();
        let ty = e.file_type()?;
        if ty.is_symlink() || name.ends_with(".AppImage") || (ty.is_file() && is_save(&name)) {
            skipped += usize::from(!name.ends_with(".AppImage"));
            continue;
        }
        if ty.is_dir() {
            skipped += copy_game(&e.path(), &dst.join(&name))?;
        } else {
            std::fs::copy(e.path(), dst.join(&name))?;
        }
    }
    Ok(skipped)
}

/// Make one AppImage per selected Linux platform of `opts`, next to the game
/// folder. Engines must be built already. Returns the files made.
pub fn create(game_dir: &Path, opts: &Options, log: &(dyn Fn(String) + Sync)) -> Result<Vec<PathBuf>, String> {
    check_tools()?;
    let linux: Vec<Platform> = opts.platforms.iter().copied().filter(|p| arch(*p).is_some()).collect();
    if linux.is_empty() {
        return Err("AppImages need a Linux engine: choose at least one Linux platform.".into());
    }
    let mut made = Vec::new();
    for p in linux {
        let a = arch(p).unwrap();
        let plan = Plan::new(game_dir, &Options { platforms: vec![p], ..opts.clone() })?;
        if !plan.ready() {
            return Err(format!("the {} engine isn't built yet", p.label()));
        }
        let out = output_path(game_dir, &plan.title, p);
        let parent = out.parent().unwrap_or(Path::new("."));
        let stage = parent.join(format!(".{}.AppDir", out.file_name().unwrap_or_default().to_string_lossy()));
        let _ = util::remove_path(&stage);
        let result = (|| -> Result<(), String> {
            let copy = stage.join("game");
            log(format!("Copying the game for the {} AppImage ...", a));
            let skipped = copy_game(game_dir, &copy).map_err(|e| format!("copying the game failed: {}", e))?;
            if skipped > 0 {
                log(format!("Left out {} save file(s) and link(s): the AppImage starts without saves.", skipped));
            }
            let mut plan = Plan::new(&copy, &Options { platforms: vec![p], ..opts.clone() })?;
            // the RTP found on this computer goes into the AppImage
            plan.bundle_rtp = true;
            crate::patch::apply(&mut plan, &|l| log(l))?;
            // the player's own mkxp-z settings and key bindings (kept by the
            // patch) don't go into an AppImage that may be passed on
            let shared = copy.join(crate::plan::ENGINE_SUBDIR);
            for e in std::fs::read_dir(&shared).into_iter().flatten().flatten() {
                if crate::patch::is_player_file(&e.file_name().to_string_lossy()) {
                    let _ = std::fs::remove_file(e.path());
                }
            }

            let id = safe_name(&plan.title).replace(' ', "_");
            let icon_png = plan.icon_png.clone()
                .or_else(|| crate::pe::icon_from_ico(crate::patch::MKXPZ_ICON).and_then(|i| i.to_png()))
                .ok_or("no icon")?;
            let io = |e: std::io::Error| e.to_string();
            std::fs::write(stage.join(format!("{}.png", id)), &icon_png).map_err(io)?;
            std::fs::write(stage.join(".DirIcon"), &icon_png).map_err(io)?;
            std::fs::write(stage.join(format!("{}.desktop", id)), desktop_file(&plan.title, &id)).map_err(io)?;
            let apprun = stage.join("AppRun");
            std::fs::write(&apprun, APPRUN.replace("__DATADIR__", &safe_name(&plan.title))).map_err(io)?;
            util::make_executable(&apprun).map_err(io)?;

            let image = parent.join(format!(".{}.squashfs", out.file_name().unwrap_or_default().to_string_lossy()));
            squash(&stage, &image, log)?;
            let rt = runtime(&opts.paths, a, log);
            let data = std::fs::read(&image).map_err(io);
            let _ = std::fs::remove_file(&image);
            let mut file = rt?;
            file.extend_from_slice(&data?);
            std::fs::write(&out, &file).map_err(|e| format!("{}: {}", out.display(), e))?;
            util::make_executable(&out).map_err(io)?;
            Ok(())
        })();
        let _ = util::remove_path(&stage);
        result?;
        log(format!("AppImage ready: {} ({} MB)", out.display(),
                    std::fs::metadata(&out).map(|m| m.len() / 1_000_000).unwrap_or(0)));
        made.push(out);
    }
    Ok(made)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names() {
        assert_eq!(safe_name("Dazed: The Game/2"), "Dazed_ The Game_2");
        assert_eq!(safe_name("  "), "Game");
        let o = output_path(Path::new("/g/Silenced Demo"), "Silenced", Platform::Linux64);
        assert_eq!(o, PathBuf::from("/g/Silenced-x86_64.AppImage"));
        assert!(APPRUN.contains("cp -rsn") && desktop_file("A", "A").contains("Icon=A"));
        assert!(is_save("Save01.rvdata2") && is_save("SAVE1.RXDATA") && !is_save("Save.ogg") && !is_save("save-history.rb"));
    }
}
