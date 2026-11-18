// No console window when started from Explorer; the CLI re-attaches to the
// parent console (see attach_console) so command-line use still prints.
#![cfg_attr(windows, windows_subsystem = "windows")]

use mkxpz_patcher::engines::{self, Paths, Platform};
use mkxpz_patcher::{patch, plan};
#[cfg(all(not(windows), feature = "gtk"))]
use mkxpz_patcher::gui;
#[cfg(windows)]
use mkxpz_patcher::gui_win as gui;
use std::path::PathBuf;

const HELP: &str = "\
MKXP-Z Patcher - run RPG Maker XP / VX / VX Ace games on the mkxp-z engine,
natively on Linux and Windows (x86 32/64-bit, ARM).

Usage:
  mkxpz-patcher                        open the window
  mkxpz-patcher [options] GAME_FOLDER  patch from the command line

Options:
  --platform LIST     engines to add, comma-separated:
                        linux, linux32, windows (32-bit), windows64,
                        linux-arm64, linux-armhf (32-bit ARM), windows-arm64
                        both = linux,windows   all = linux,linux32,windows
                        arm = linux-arm64,linux-armhf,windows-arm64
                      (default: this computer's platform)
  --appimage          pack the game with its Linux engine into an AppImage
                      (one per Linux platform chosen, next to the game folder;
                      the game folder itself stays as it is)
  --check             only analyze the game and print the report
  --unpatch           remove a previously applied patch
  --no-build          don't build missing engines (fail instead)
  --build-engines LIST  only build these engines (same names as --platform)
  --fetch-source      only download the mkxp-z source (if it isn't there yet)
  --source-archive F  take the mkxp-z source from this .tar.gz instead of
                      downloading it (with --fetch-source, or when building)
  --no-soundfont      don't bundle a SoundFont for MIDI music in Windows patches
  --keep-exe          Windows: leave Game.exe as it is and add the launcher next
                      to it as \"Game (mkxp-z).exe\" (default: the launcher becomes
                      Game.exe, the original is kept as Game.original.exe; Steam
                      versions then need launch options, see mkxp-z/README.txt)
  --engines DIR       folder with the built engines (default: see below)
  --mkxpz-src DIR     mkxp-z source folder (fetched automatically if missing)
  --msys2 DIR         Windows: folder for the portable MSYS2
  --engine DIR        use this Linux 64-bit mkxp-z build/install folder
  --rtp DIR           the matching RTP folder, if the game needs one
  --rgss N            force the RGSS version (1, 2 or 3)
  --gui               open the window with GAME_FOLDER preselected
  -h, --help          this help
  --version

Folders, by default next to the patcher program (only if that folder isn't
writable: ~/.local/share/mkxpz-patcher or %LOCALAPPDATA%\\mkxpz-patcher):
  mkxp-z/          built engines, one folder per platform
  mkxp-z_source/   mkxp-z source code and its build folders (fetched when needed)
  msys2/           Windows only: portable MSYS2 with the compilers (set up when needed;
                   on a network or shared drive it goes to %LOCALAPPDATA%\\mkxpz-patcher)

Engines are built on demand. Linux builds all of them (Windows ones are cross-
compiled with MinGW-w64, Windows ARM64 with llvm-mingw, which is downloaded;
Linux ARM ones with the distribution's ARM cross compiler, or in a Debian
container with podman/docker where there is none, e.g. on Fedora); Windows
builds the x86 Windows engines in the portable MSYS2.
";

fn host_platform() -> Platform {
    match Platform::host() {
        // RPG Maker games are 32-bit Windows programs: the 32-bit engine fits
        // them best on x86 Windows (e.g. their Win32API calls)
        Some(Platform::Windows64) | None if cfg!(windows) => Platform::Windows32,
        Some(p) => p,
        None => Platform::Linux64,
    }
}

pub fn parse_platforms(list: &str) -> Result<Vec<Platform>, String> {
    let mut out = Vec::new();
    for k in list.split(',').map(str::trim).filter(|s| !s.is_empty()) {
        match k {
            "both" => out.extend([Platform::Linux64, Platform::Windows32]),
            "all" => out.extend([Platform::Linux64, Platform::Linux32, Platform::Windows32]),
            "arm" => out.extend([Platform::LinuxArm64, Platform::LinuxArmhf, Platform::WindowsArm64]),
            _ => out.push(Platform::from_key(k).ok_or_else(|| format!("unknown platform '{}'", k))?),
        }
    }
    Ok(out)
}

#[cfg(windows)]
fn attach_console() {
    unsafe {
        windows_sys::Win32::System::Console::AttachConsole(windows_sys::Win32::System::Console::ATTACH_PARENT_PROCESS);
    }
}
#[cfg(not(windows))]
fn attach_console() {}

struct Args {
    game: Option<PathBuf>,
    platforms: Option<Vec<Platform>>,
    check: bool,
    fetch_source: bool,
    source_archive: Option<PathBuf>,
    unpatch: bool,
    no_build: bool,
    build_only: Option<Vec<Platform>>,
    no_soundfont: bool,
    keep_exe: bool,
    engines: Option<PathBuf>,
    src: Option<PathBuf>,
    msys2: Option<PathBuf>,
    engine: Option<PathBuf>,
    rtp: Option<PathBuf>,
    rgss: Option<usize>,
    gui: bool,
    appimage: bool,
}

fn parse_args() -> Result<Args, String> {
    let mut a = Args { game: None, platforms: None, check: false, fetch_source: false, source_archive: None, unpatch: false, no_build: false, build_only: None,
                       no_soundfont: false, keep_exe: false, engines: None, src: None, msys2: None, engine: None, rtp: None, rgss: None, gui: false, appimage: false };
    let mut it = std::env::args_os().skip(1);
    while let Some(arg) = it.next() {
        let s = arg.to_string_lossy().into_owned();
        let mut val = |name: &str| it.next().ok_or_else(|| format!("{} needs a value", name));
        match s.as_str() {
            "-h" | "--help" => {
                print!("{}", HELP);
                std::process::exit(0);
            }
            "--version" => {
                println!("MKXP-Z Patcher {}", env!("CARGO_PKG_VERSION"));
                std::process::exit(0);
            }
            "--platform" => a.platforms = Some(parse_platforms(&val(&s)?.to_string_lossy())?),
            "--build-engines" => a.build_only = Some(parse_platforms(&val(&s)?.to_string_lossy())?),
            "--engines" => a.engines = Some(val(&s)?.into()),
            "--mkxpz-src" => a.src = Some(val(&s)?.into()),
            "--msys2" => a.msys2 = Some(val(&s)?.into()),
            "--engine" => a.engine = Some(val(&s)?.into()),
            "--rtp" => a.rtp = Some(val(&s)?.into()),
            "--rgss" => a.rgss = Some(val(&s)?.to_string_lossy().parse().ok().filter(|n| (1..=3).contains(n))
                .ok_or("--rgss must be 1, 2 or 3")?),
            "--check" => a.check = true,
            "--fetch-source" => a.fetch_source = true,
            "--source-archive" => a.source_archive = Some(val(&s)?.into()),
            "--unpatch" => a.unpatch = true,
            "--no-build" => a.no_build = true,
            "--no-soundfont" => a.no_soundfont = true,
            "--keep-exe" => a.keep_exe = true,
            "--gui" => a.gui = true,
            "--appimage" => a.appimage = true,
            _ if s.starts_with('-') => return Err(format!("unknown option {} (see --help)", s)),
            _ if a.game.is_none() => a.game = Some(arg.into()),
            _ => return Err(format!("unexpected argument {}", s)),
        }
    }
    Ok(a)
}

fn main() {
    mkxpz_patcher::install_crash_log();
    let args = std::env::args_os().count();
    if args > 1 {
        attach_console();
    }
    let a = match parse_args() {
        Ok(a) => a,
        Err(e) => {
            eprintln!("error: {}", e);
            std::process::exit(2);
        }
    };
    // Command-line paths are relative to the current directory (unlike the
    // window's fields, which are relative to the patcher's folder).
    let abs = |p: &PathBuf| if p.is_absolute() { p.clone() } else { std::env::current_dir().unwrap_or_default().join(p) };
    let mut a = a;
    for p in [&mut a.engines, &mut a.src, &mut a.msys2, &mut a.engine, &mut a.rtp, &mut a.game, &mut a.source_archive]
        .into_iter().flatten() {
        *p = abs(p);
    }
    let mut paths = Paths::default_paths();
    if let Some(e) = &a.engines {
        paths.engines = e.clone();
    }
    if let Some(s) = &a.src {
        paths.source = s.clone();
    }
    if let Some(m) = &a.msys2 {
        paths.msys2 = m.clone();
    }

    if a.fetch_source {
        if let Err(e) = engines::ensure_source_from(&paths.resolved().source, a.source_archive.as_deref(), &log_line) {
            eprintln!("error: {}", e);
            std::process::exit(1);
        }
        return;
    }
    if a.build_only.is_none() && (a.gui || a.game.is_none()) {
        if let Err(e) = run_window(&a, paths) {
            attach_console();
            eprintln!("error: couldn't open the window: {}", e);
            std::process::exit(1);
        }
        return;
    }
    if let Err(e) = cli(a, paths) {
        eprintln!("error: {}", e);
        std::process::exit(1);
    }
}

#[cfg(any(windows, feature = "gtk"))]
fn run_window(a: &Args, paths: Paths) -> Result<(), String> {
    gui::run(a.game.clone(), paths, a.engine.clone())
}

/// Built without GTK (e.g. the static ARM builds): command line only.
#[cfg(not(any(windows, feature = "gtk")))]
fn run_window(_: &Args, _: Paths) -> Result<(), String> {
    print!("{}", HELP);
    Err("this build has no window; use it from the command line (see above)".into())
}

fn log_line(s: String) {
    println!("{}", s);
}

fn build_missing(missing: &[Platform], paths: &Paths, archive: Option<&std::path::Path>) -> Result<(), String> {
    engines::check_build_tools(missing)?;
    engines::ensure_source_from(&paths.resolved().source, archive, &log_line)?;
    for &p in missing {
        eprintln!("Building the {} engine (the first build takes a while) ...", p.label());
        engines::build(p, paths, &log_line)?;
    }
    Ok(())
}

fn cli(a: Args, paths: Paths) -> Result<(), String> {
    if let Some(list) = &a.build_only {
        return build_missing(list, &paths, a.source_archive.as_deref());
    }
    let game = a.game.clone().unwrap();
    if a.unpatch {
        return patch::unpatch(&game, &log_line);
    }
    let opts = plan::Options {
        platforms: a.platforms.clone().unwrap_or_else(|| vec![host_platform()]),
        paths: paths.clone(),
        linux_engine: a.engine.clone(),
        rtp: a.rtp.clone(),
        rgss: a.rgss,
        bundle_soundfont: !a.no_soundfont,
        replace_exe: !a.keep_exe,
    };
    eprintln!("Analyzing {} ...", game.display());
    let mut p = plan::Plan::new(&game, &opts)?;
    println!("{}", p.summary());
    if a.check {
        return Ok(());
    }
    if !p.errors.is_empty() {
        return Err(p.errors.join("\n"));
    }
    println!();
    if !p.ready() {
        if a.no_build {
            return Err(format!("missing engines: {} (run without --no-build to build them)",
                               p.missing_engines.iter().map(|x| x.target()).collect::<Vec<_>>().join(", ")));
        }
        build_missing(&p.missing_engines.clone(), &paths, a.source_archive.as_deref())?;
        p = plan::Plan::new(&game, &opts)?;
    }
    if a.appimage {
        mkxpz_patcher::appimage::create(&game, &opts, &log_line)?;
        return Ok(());
    }
    patch::apply(&mut p, &log_line)
}
