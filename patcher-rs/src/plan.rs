//! Analysis of a game folder: everything decided before anything is written.

use crate::engines::{self, Engine, Paths, Platform};
use crate::fonts::{self, FontReport, CJK_FONT_FILE};
use crate::game::{self, GameFiles, RGSS_NAMES};
use crate::patch::{self, Manifest};
use std::collections::HashSet;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

pub const ENGINE_SUBDIR: &str = "mkxp-z";
/// Patcher-made compatibility scripts (preloaded from <game>/mkxp-z/compat/).
const COMPAT_SCRIPTS: [(&str, &str); 5] = [
    ("no_rgss_memory.rb", include_str!("../assets/compat/no_rgss_memory.rb")),
    ("bitmap_marshal.rb", include_str!("../assets/compat/bitmap_marshal.rb")),
    ("fullscreen_plus.rb", include_str!("../assets/compat/fullscreen_plus.rb")),
    ("steam_log.rb", include_str!("../assets/compat/steam_log.rb")),
    ("widescreen.rb", include_str!("../assets/compat/widescreen.rb")),
];

pub fn compat_script(name: &str) -> Option<&'static str> {
    COMPAT_SCRIPTS.iter().find(|(n, _)| *n == name).map(|(_, t)| *t)
}
const STANDARD_DLLS: [&str; 12] = ["user32", "kernel32", "gdi32", "advapi32", "shell32", "winmm", "ole32",
                                   "msvcrt", "imm32", "comdlg32", "steam_api", "steam_api64"];

#[derive(Clone)]
pub struct Options {
    pub platforms: Vec<Platform>,
    pub paths: Paths,
    /// use this Linux 64-bit engine (build or install dir) instead of the engines folder
    pub linux_engine: Option<PathBuf>,
    pub rtp: Option<PathBuf>,
    pub rgss: Option<usize>,
    pub bundle_soundfont: bool,
    /// Windows: the launcher takes the place of Game.exe (the original becomes
    /// Game.original.exe); else it's added next to it as "Game.mkxp-z.exe"
    /// (64-bit engines: "Game.mkxp-z.64.exe")
    pub replace_exe: bool,
}

pub struct Plan {
    pub game_dir: PathBuf,
    pub platforms: Vec<Platform>,
    pub rgss: usize,
    pub title: String,
    pub exec_name: String,
    pub archive: Option<PathBuf>,
    pub manifest: Option<Manifest>,
    pub engines: Vec<Engine>,
    /// selected platforms whose engine isn't built yet
    pub missing_engines: Vec<Platform>,
    pub scripts_src: Option<PathBuf>,
    pub license_src: Option<PathBuf>,
    pub preload: Vec<String>,
    /// compat script file names (see COMPAT_SCRIPTS)
    pub compat: Vec<String>,
    pub fonts: Option<FontReport>,
    pub cjk_font: Option<PathBuf>,
    pub rtp: Option<PathBuf>,
    pub soundfont: Option<PathBuf>,
    pub notes: Vec<String>,
    pub warnings: Vec<String>,
    /// reasons this choice of platforms can't work: patching is refused
    pub errors: Vec<String>,
    /// the game's icon as PNG, for mkxp-z's window (set while patching)
    pub icon_png: Option<Vec<u8>>,
    /// copy the RTP into the game (mkxp-z/RTP) instead of using it where it
    /// is on this computer: for AppImages, which run on other computers too
    pub bundle_rtp: bool,
    /// see Options::replace_exe
    pub replace_exe: bool,
}

/// The slow part (scripts + all text), cached per game state so that changing
/// platform options in the GUI is instant.
struct Content {
    scripts: Option<Vec<(String, Vec<u8>)>>,
    chars: Option<HashSet<char>>,
}

type CacheKey = (PathBuf, usize, Vec<Option<std::time::SystemTime>>);
static CACHE: Mutex<Option<(CacheKey, Arc<Content>)>> = Mutex::new(None);

fn content(dir: &Path, archive: Option<&Path>, rgss: usize) -> Arc<Content> {
    let mut stamps = Vec::new();
    for p in archive.into_iter().map(Path::to_path_buf).chain([dir.join("Data"), dir.join("Fonts")]) {
        stamps.push(std::fs::metadata(&p).and_then(|m| m.modified()).ok());
    }
    let key = (dir.to_path_buf(), rgss, stamps);
    if let Some((k, c)) = CACHE.lock().unwrap().as_ref() {
        if *k == key {
            return c.clone();
        }
    }
    let files = GameFiles::new(dir, archive);
    let scripts = game::read_scripts(&files, rgss);
    let chars = files.readable.then(|| game::collect_text_chars(&files, rgss));
    let c = Arc::new(Content { scripts, chars });
    *CACHE.lock().unwrap() = Some((key, c.clone()));
    c
}

/// Installed .sf2 SoundFonts, best first. (mkxp-z's fluidsynth is built without
/// libsndfile, so it can't read compressed .sf3 files.)
pub fn find_soundfonts() -> Vec<PathBuf> {
    let home = crate::util::home_dir();
    let roots = [PathBuf::from("/usr/share/soundfonts"), PathBuf::from("/usr/share/sounds/sf2"),
                 PathBuf::from("/usr/local/share/soundfonts"), home.join(".local/share/soundfonts")];
    let preferred = ["FluidR3_GM.sf2", "default-GM.sf2", "default.sf2", "TimGM6mb.sf2"];
    let mut out: Vec<PathBuf> = Vec::new();
    for r in roots.iter().filter(|r| r.is_dir()) {
        let mut found: Vec<PathBuf> = preferred.iter().map(|n| r.join(n)).filter(|p| p.is_file()).collect();
        let mut rest: Vec<PathBuf> = std::fs::read_dir(r).into_iter().flatten().flatten().map(|e| e.path())
            .filter(|p| p.is_file() && p.extension().map_or(false, |x| x.eq_ignore_ascii_case("sf2"))).collect();
        rest.sort();
        found.extend(rest);
        for p in found {
            let p = p.canonicalize().unwrap_or(p);
            if !out.contains(&p) {
                out.push(p);
            }
        }
    }
    out
}

fn find_dlls(blob: &str) -> Vec<String> {
    let mut out = HashSet::new();
    let lower = blob.to_lowercase();
    // Win32API.new('name', ...) / Win32API.new("name.dll", ...)
    for (i, _) in lower.match_indices("win32api.new(") {
        let rest = lower[i + 13..].trim_start();
        if let Some(q) = rest.chars().next().filter(|c| *c == '\'' || *c == '"') {
            let name: String = rest[1..].chars().take_while(|c| *c != q).collect();
            out.insert(name.trim_end_matches(".dll").to_string());
        }
    }
    // any "something.dll" literal
    for (i, _) in lower.match_indices(".dll") {
        let after = lower[i + 4..].chars().next();
        if !matches!(after, Some('"') | Some('\'')) {
            continue;
        }
        let name: String = lower[..i].chars().rev()
            .take_while(|c| c.is_ascii_alphanumeric() || "_-.".contains(*c)).collect::<String>()
            .chars().rev().collect();
        let quoted = lower[..i - name.len()].ends_with('"') || lower[..i - name.len()].ends_with('\'');
        if quoted && !name.is_empty() {
            out.insert(name);
        }
    }
    let mut v: Vec<String> = out.into_iter()
        .filter(|d| !d.is_empty() && !STANDARD_DLLS.contains(&d.as_str()) && !d.starts_with("rgss"))
        .collect();
    v.sort();
    v
}

impl Plan {
    pub fn new(game_dir: &Path, o: &Options) -> Result<Plan, String> {
        let game_dir = game_dir.canonicalize().map_err(|e| format!("{}: {}", game_dir.display(), e))?;
        if !game_dir.is_dir() {
            return Err(format!("Not a folder: {}", game_dir.display()));
        }
        let mut platforms = o.platforms.clone();
        platforms.sort();
        platforms.dedup();
        if platforms.is_empty() {
            return Err("Choose at least one platform.".into());
        }
        if platforms.contains(&Platform::Windows32) && platforms.contains(&Platform::Windows64) {
            return Err("Choose either the 32-bit or the 64-bit Windows engine, not both.".into());
        }
        let (ini_path, ini) = game::read_game_ini(&game_dir);
        let rgss = o.rgss.unwrap_or_else(|| game::detect_rgss(&game_dir, &ini));
        if rgss == 0 {
            return Err(format!("This doesn't look like an RPG Maker XP/VX/VX Ace game\n(no Game.ini, \
                                Data/Scripts.r*data or .rgss*a archive found):\n{}", game_dir.display()));
        }
        let title = ini.get("Title").cloned()
            .unwrap_or_else(|| game_dir.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default());
        let exec_name = ini_path.as_ref().and_then(|p| p.file_stem()).map(|s| s.to_string_lossy().into_owned())
            .unwrap_or_else(|| "Game".into());
        let archive = game::find_archive(&game_dir, rgss);
        let manifest = patch::read_manifest(&game_dir)?;
        let (mut notes, mut warnings, mut errors) = (Vec::new(), Vec::new(), Vec::new());

        // engines
        let paths = o.paths.resolved();
        let mut engines = Vec::new();
        let mut missing_engines = Vec::new();
        for &p in &platforms {
            let explicit = if p == Platform::Linux64 { o.linux_engine.as_deref() } else { None };
            match engines::locate(p, &paths, explicit) {
                Ok(e) => engines.push(e),
                Err(_) => missing_engines.push(p),
            }
        }
        let scripts_src = engines.iter().find_map(|e| e.scripts.clone());
        let license_src = engines.iter().find_map(|e| e.license.clone());
        let has = |p: Platform| platforms.contains(&p);
        let any_linux = platforms.iter().any(|p| p.is_linux());
        let any_win = platforms.iter().any(|p| p.is_windows());
        let any_android = platforms.iter().any(|p| p.is_android());
        if any_android {
            notes.push(format!("Android: the game goes into an APK next to the game folder (the folder itself stays as \
                                it is). Install it on Android 8.0 or newer; the first start copies the game into the \
                                app's storage, where it keeps its saves. Without a keyboard or controller there are \
                                no controls yet{}.", if has(Platform::AndroidX64) && !has(Platform::AndroidArm64) {
                                    " (x86 64-bit: the Android emulator, Waydroid, some Chromebooks; phones need ARM 64-bit)"
                                } else { "" }));
        }
        if has(Platform::WindowsXP) {
            notes.push("Windows XP: the legacy engine is added (mkxp-z.xp.exe). Game.exe starts it on Windows XP, \
                        and on newer Windows too unless a normal Windows engine is there. It draws with OpenGL (the graphics driver needs OpenGL \
                        2.0) and has no HTTPS or Ruby sockets.".into());
        }

        // scripts
        let c = content(&game_dir, archive.as_deref(), rgss);
        let blob: Option<String> = c.scripts.as_ref()
            .map(|s| s.iter().map(|(_, src)| String::from_utf8_lossy(src)).collect::<Vec<_>>().join("\n"));
        let mut preload = Vec::new();
        let mut compat: Vec<String> = Vec::new();
        let uses_win32 = blob.as_ref().map_or(true, |b| b.contains("Win32API"));
        // RGSS1-3 ran on Ruby 1.8/1.9; restore methods those scripts expect.
        preload.push(format!("{}/scripts/preload/ruby_classic_wrap.rb", ENGINE_SUBDIR));
        if uses_win32 {
            // Linux: emulates common calls. Windows: passes them on to the real API.
            preload.push(format!("{}/scripts/preload/win32_wrap.rb", ENGINE_SUBDIR));
        }
        match &blob {
            None => notes.push("Scripts are unreadable; Win32API support enabled just in case.".into()),
            Some(_) if uses_win32 && any_linux =>
                notes.push("Scripts use Win32API; on Linux common calls are emulated, some features may not work.".into()),
            _ => {}
        }
        if let Some(b) = &blob {
            if b.to_lowercase().contains("steam_api") && b.contains("SteamAPI_Init") {
                // off unless MKXPZ_STEAM_LOG is set (see the script / README)
                compat.push("steam_log.rb".into());
                if !has(Platform::Windows32) {
                    // Steam starts Game.exe (or, with launch options, the launcher); only the
                    // 32-bit Windows engine can run there
                    errors.push("This is a Steam version: it only runs through Steam with the 32-bit Windows \
                                 engine (on Linux via Proton). Choose \"Windows\" with \"32-bit\"; without it Steam keeps \
                                 starting the original Game.exe and the patch is never used.".into());
                }
                if any_linux {
                    warnings.push("Linux: this is a Steam version. Its scripts call SteamAPI_Init through steam_api.dll \
                                   and quit if that fails; the Windows DLL can't load on Linux, so the Linux engine \
                                   will exit right at startup. Play it via Steam (Proton) with the 32-bit Windows \
                                   engine.".into());
                }
                if has(Platform::Windows32) && o.replace_exe {
                    notes.push(format!("Windows: Steam version - the game's own steam_api.dll is called natively, so Steam \
                                must be running. {0}.exe is replaced by the mkxp-z launcher, so Steam starts mkxp-z as \
                                before (on Linux via Proton); no launch options needed.", exec_name));
                } else if has(Platform::Windows32) {
                    notes.push(format!("Windows: Steam version - the game's own steam_api.dll is called natively, so Steam \
                                must be running. {0}.exe stays as it is, so Steam needs these launch options to start \
                                mkxp-z - on Windows: {1}  - on Linux (Proton): {2}  (started directly, Steam restarts \
                                the original {0}.exe).", exec_name, crate::patch::WINDOWS_LAUNCH_OPTIONS,
                                crate::patch::proton_launch_options(&exec_name, &format!("{}.exe",
                                    side_by_side_launcher(&exec_name, Platform::Windows32)))));
                }
                if has(Platform::Windows64) {
                    warnings.push("Windows 64-bit: the Steam script passes pointers as 32-bit integers and will likely \
                                   crash with the 64-bit engine. Use the 32-bit Windows engine.".into());
                }
                if has(Platform::WindowsXP) {
                    warnings.push("Windows XP: Steam no longer runs on Windows XP (since 2019), so this Steam \
                                   version can't be played there.".into());
                }
                if has(Platform::WindowsArm64) {
                    warnings.push("Windows ARM64: the game's steam_api.dll is x86 code, which the ARM64 engine can't \
                                   load, so the game quits at startup. Use the 32-bit Windows engine (Windows on ARM \
                                   runs it emulated).".into());
                }
            }
            let custom = find_dlls(b);
            if !custom.is_empty() {
                let mut why = Vec::new();
                if any_linux {
                    why.push("they can't run on Linux");
                }
                if has(Platform::Windows64) {
                    why.push("the 64-bit Windows engine can't load these 32-bit DLLs");
                }
                if has(Platform::WindowsArm64) {
                    why.push("the Windows ARM64 engine can't load these x86 DLLs");
                }
                if !why.is_empty() {
                    warnings.push(format!("Scripts load custom DLLs ({}): {}, so related features may fail.",
                                          custom.join(", "), why.join("; ")));
                }
                if has(Platform::Windows32) {
                    notes.push(format!("Windows 32-bit: the game's DLLs ({}) are called natively.", custom.join(", ")));
                }
            }
            if b.contains("RtlMoveMemory") && b.contains("object_id") {
                compat.push("no_rgss_memory.rb".into());
                if b.contains("def _dump") && b.contains("_load") && b.contains("class Bitmap") {
                    // the "fast Bitmap Marshal" trick (screenshot save systems)
                    compat.push("bitmap_marshal.rb".into());
                    notes.push("Scripts save Bitmaps by copying RGSS's internal memory (RtlMoveMemory on object \
                                addresses, e.g. screenshots in save files). That's redone with mkxp-z's own pixel \
                                access, in the original format, so save files stay compatible with Game.exe.".into());
                } else {
                    notes.push("Scripts read RGSS's internal memory (RtlMoveMemory on object addresses). Those \
                                calls are turned into no-ops (they'd crash on Windows); what they read stays \
                                empty.".into());
                }
            }
            if b.contains("Zeus_Fullscreen") && b.contains("zeus_fullscreen_update") {
                compat.push("fullscreen_plus.rb".into());
                notes.push("Fullscreen++ (Zeus81) found: F5 (fullscreen) and F6 (window size) are redirected to \
                            mkxp-z's own window handling; settings stay in Game.ini.".into());
            }
        }

        // VX / VX Ace: widescreen mode, off until chosen in the F1 menu
        if rgss >= 2 {
            compat.push("widescreen.rb".into());
        }
        for c in &compat {
            preload.push(format!("{}/compat/{}", ENGINE_SUBDIR, c));
        }

        // fonts
        let cjk = engines.iter().find_map(|e| e.asset(CJK_FONT_FILE))
            .or_else(|| Some(paths.source.join("assets").join(CJK_FONT_FILE)).filter(|p| p.is_file()));
        let builtin = engines.iter().find_map(|e| e.asset("liberation.ttf"))
            .or_else(|| Some(paths.source.join("assets/liberation.ttf")).filter(|p| p.is_file()));
        let mut fonts_report = None;
        let mut cjk_font = None;
        if let Some(chars) = &c.chars {
            let r = fonts::analyze(&game_dir, c.scripts.as_deref(), rgss, chars, cjk.as_deref(), builtin.as_deref());
            if !r.missing.is_empty() {
                let sample: String = r.missing.iter().take(12).collect();
                let desc = r.used.clone().unwrap_or_else(|| "mkxp-z's built-in font".into());
                if let Some(f) = &r.fix {
                    cjk_font = Some(f.clone());
                    notes.push(format!("The game's text uses {} characters {} can't show (e.g. {}); substituting \
                                        WenQuanYi Micro Hei.", r.missing.len(), desc, sample));
                } else {
                    warnings.push(format!("The game's text uses {} characters {} can't show (e.g. {}), and no \
                                           replacement font was found; they will appear as boxes.",
                                          r.missing.len(), desc, sample));
                }
            }
            fonts_report = Some(r);
        }

        // RTP
        let mut rtp = None;
        if game::needs_rtp(&game_dir, &ini, rgss) {
            rtp = game::find_rtp(rgss, o.rtp.as_deref())?;
            match &rtp {
                Some(p) => notes.push(format!("Using the RTP at {} (only for engines of this computer's OS: it's an \
                                               absolute path)", p.display())),
                None => warnings.push(format!("The game seems to need the {} RTP, which wasn't found. Choose its \
                                               folder, or graphics/sounds may be missing.",
                                              RGSS_NAMES[rgss].split(" (").next().unwrap_or(""))),
            }
        } else if o.rtp.is_some() {
            rtp = game::find_rtp(rgss, o.rtp.as_deref())?;
        }

        // MIDI
        let mut soundfont = None;
        if game::has_midi(&game_dir) {
            // Windows and Android have no SoundFont of their own
            let bundles = if any_android { "Android" } else { "Windows" };
            if (any_win || any_android) && o.bundle_soundfont {
                match find_soundfonts().into_iter().next() {
                    Some(sf) => {
                        let mb = std::fs::metadata(&sf).map(|m| m.len() >> 20).unwrap_or(0);
                        notes.push(format!("Uses MIDI music: bundling SoundFont {} ({} MB) for {}.",
                                           sf.file_name().unwrap_or_default().to_string_lossy(), mb, bundles));
                        soundfont = Some(sf);
                    }
                    None => warnings.push(format!("Uses MIDI music, but no .sf2 SoundFont is installed here to bundle \
                                                   for {}; MIDI music stays silent there (install one, e.g. \
                                                   fluid-soundfont-gm, or put one at mkxp-z/soundfont.sf2).", bundles)),
                }
            } else if any_win {
                notes.push("Uses MIDI music: put a SoundFont at mkxp-z\\soundfont.sf2 for Windows.".into());
            }
            if any_linux && soundfont.is_none() {
                notes.push("Uses MIDI music: on Linux the launcher links an installed SoundFont at start \
                            (package fluid-soundfont-gm).".into());
            }
        }
        if has(Platform::Linux32) {
            notes.push("Linux 32-bit: on 64-bit systems Game.sh prefers the 64-bit engine; the 32-bit one runs there \
                        only with 32-bit glibc installed (Fedora: glibc.i686).".into());
        }

        Ok(Plan { game_dir, platforms, rgss, title, exec_name, archive, manifest, engines, missing_engines,
                  scripts_src, license_src, preload, compat, fonts: fonts_report, cjk_font, rtp, soundfont,
                  notes, warnings, errors, icon_png: None, bundle_rtp: false, replace_exe: o.replace_exe })
    }

    pub fn engine(&self, p: Platform) -> Option<&Engine> {
        self.engines.iter().find(|e| e.platform == p)
    }

    pub fn patched(&self) -> bool {
        self.manifest.is_some()
    }

    pub fn ready(&self) -> bool {
        self.missing_engines.is_empty()
    }

    /// The Windows launcher takes the place of Game.exe (the original is backed
    /// up and restored on unpatch), so shortcuts and Steam start mkxp-z; or,
    /// without replace_exe, it's added next to it as Game.mkxp-z.exe (32-bit
    /// engines) or Game.mkxp-z.64.exe (64-bit and ARM64).
    pub fn launcher_name(&self, p: Platform) -> String {
        if !p.is_windows() {
            return format!("{}.sh", self.exec_name);
        }
        let ext = if crate::patch::have_exe_launcher() { "exe" } else { "cmd" };
        if self.replace_exe && ext == "exe" {
            format!("{}.exe", self.exec_name)
        } else {
            format!("{}.{}", side_by_side_launcher(&self.exec_name, p), ext)
        }
    }

    /// Distinct launchers (both Linux engines share one Game.sh). Android has
    /// none: its APK starts the game (android.rs).
    pub fn launchers(&self) -> Vec<(Platform, String)> {
        let mut out: Vec<(Platform, String)> = Vec::new();
        for &p in self.platforms.iter().filter(|p| !p.is_android()) {
            let n = self.launcher_name(p);
            if !out.iter().any(|(_, x)| *x == n) {
                out.push((p, n));
            }
        }
        out
    }

    pub fn summary(&self) -> String {
        let status = match &self.manifest {
            None => "not patched yet".to_string(),
            Some(m) if !m.files.iter().any(|f| self.game_dir.join(f).exists()) =>
                "only a leftover patch record - its files are gone; \"Remove patch\" clears it".to_string(),
            Some(m) => format!("already patched ({})", m.platforms.iter()
                .filter_map(|k| Platform::from_key(k)).map(|p| p.label()).collect::<Vec<_>>().join(", ")),
        };
        let mut lines = vec![
            format!("Game:     {}", self.title),
            format!("Engine:   {}", RGSS_NAMES[self.rgss]),
            format!("Data:     {}", self.archive.as_ref()
                .map(|a| format!("{} (encrypted)", a.file_name().unwrap_or_default().to_string_lossy()))
                .unwrap_or_else(|| "unencrypted folders".into())),
            format!("Status:   {}", status),
        ];
        for &p in &self.platforms {
            lines.push(format!("Target:   {:<15} {}", p.label(), self.engine(p)
                .map(|e| e.dir.display().to_string()).unwrap_or_else(|| "engine not built yet".into())));
        }
        if let Some(f) = &self.fonts {
            lines.push(format!("Font:     {}{}", f.used.clone().unwrap_or_else(|| "built-in (requested fonts not shipped)".into()),
                               if self.cjk_font.is_some() { " -> WenQuanYi Micro Hei" } else { "" }));
        }
        lines.extend(self.notes.iter().map(|n| format!("Note:     {}", n)));
        lines.extend(self.warnings.iter().map(|w| format!("Warning:  {}", w)));
        lines.extend(self.errors.iter().map(|e| format!("Error:    {}", e)));
        if !self.missing_engines.is_empty() {
            lines.push(format!("Missing:  engines for {} - they'll be built first (takes a while once).",
                               self.missing_engines.iter().map(|p| p.label()).collect::<Vec<_>>().join(", ")));
        }
        lines.join("\n")
    }
}

/// The launcher's name next to the game's own Game.exe (without replace_exe).
/// "Game.mkxp-z" (64-bit and ARM64 engines: "Game.mkxp-z.64"), after the
/// game's own exe name.
pub fn side_by_side_launcher(exec_name: &str, p: Platform) -> String {
    match p {
        Platform::Windows64 | Platform::WindowsArm64 => format!("{}.mkxp-z.64", exec_name),
        _ => format!("{}.mkxp-z", exec_name),
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn dlls() {
        let s = "A = Win32API.new('user32', 'X', 'i', 'i')\nB = Win32API.new(\"MyLib.dll\", 'Y', '', '')\n\
                 load_data(\"steam_api.dll\")\nC = \"other.dll\"";
        assert_eq!(super::find_dlls(s), vec!["mylib", "other"]);
    }
}
