//! mkxp-z engines: which ones exist, where they are, and building missing ones.

use crate::util::{self, Distro};
use std::io::{BufRead, BufReader};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

pub const MKXPZ_REPO: &str = "https://github.com/mkxp-z/mkxp-z.git";
/// The mkxp-z commit this patcher was tested with (fetched when no source is around).
pub const MKXPZ_COMMIT: &str = "f02d586624dc53de373889cc16d9b1516da0a3a5";

#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug, PartialOrd, Ord)]
pub enum Platform {
    Linux64,
    Linux32,
    Windows32,
    Windows64,
    /// 64-bit ARM (aarch64), e.g. Raspberry Pi 4/5 with a 64-bit system
    LinuxArm64,
    /// 32-bit ARM with hardware floating point (armv7, Debian's "armhf")
    LinuxArmhf,
    /// Windows on ARM (aarch64)
    WindowsArm64,
}

pub const ALL_PLATFORMS: [Platform; 7] = [Platform::Linux64, Platform::Linux32, Platform::Windows32, Platform::Windows64,
                                          Platform::LinuxArm64, Platform::LinuxArmhf, Platform::WindowsArm64];

impl Platform {
    pub fn key(self) -> &'static str {
        match self {
            Platform::Linux64 => "linux",
            Platform::Linux32 => "linux32",
            Platform::Windows32 => "windows",
            Platform::Windows64 => "windows64",
            Platform::LinuxArm64 => "linux-arm64",
            Platform::LinuxArmhf => "linux-armhf",
            Platform::WindowsArm64 => "windows-arm64",
        }
    }
    pub fn from_key(k: &str) -> Option<Platform> {
        ALL_PLATFORMS.iter().copied().find(|p| p.key() == k)
            .or(match k { "windows-x86" => Some(Platform::Windows32), "windows-x64" => Some(Platform::Windows64), _ => None })
    }
    pub fn label(self) -> &'static str {
        match self {
            Platform::Linux64 => "Linux 64-bit",
            Platform::Linux32 => "Linux 32-bit",
            Platform::Windows32 => "Windows 32-bit",
            Platform::Windows64 => "Windows 64-bit",
            Platform::LinuxArm64 => "Linux ARM 64-bit",
            Platform::LinuxArmhf => "Linux ARM 32-bit",
            Platform::WindowsArm64 => "Windows ARM 64-bit",
        }
    }
    /// Name of the build target / engines subfolder.
    pub fn target(self) -> &'static str {
        match self {
            Platform::Linux64 => "linux-x86_64",
            Platform::Linux32 => "linux-i686",
            Platform::Windows32 => "windows-i686",
            Platform::Windows64 => "windows-x86_64",
            Platform::LinuxArm64 => "linux-aarch64",
            Platform::LinuxArmhf => "linux-armhf",
            Platform::WindowsArm64 => "windows-aarch64",
        }
    }
    pub fn exe(self) -> &'static str {
        match self {
            Platform::Linux64 => "mkxp-z.x86_64",
            Platform::Linux32 => "mkxp-z.i686",
            Platform::Windows32 => "mkxp-z.i686.exe",
            Platform::Windows64 => "mkxp-z.x86_64.exe",
            Platform::LinuxArm64 => "mkxp-z.aarch64",
            Platform::LinuxArmhf => "mkxp-z.armhf",
            Platform::WindowsArm64 => "mkxp-z.aarch64.exe",
        }
    }
    /// Folder inside <game>/mkxp-z/. The Windows engines share one (one launcher).
    pub fn sub(self) -> &'static str {
        match self {
            Platform::Linux64 => "linux",
            Platform::Linux32 => "linux32",
            Platform::LinuxArm64 => "linux-arm64",
            Platform::LinuxArmhf => "linux-armhf",
            Platform::Windows32 | Platform::Windows64 | Platform::WindowsArm64 => "windows",
        }
    }
    pub fn is_windows(self) -> bool {
        matches!(self, Platform::Windows32 | Platform::Windows64 | Platform::WindowsArm64)
    }
    pub fn is_arm(self) -> bool {
        matches!(self, Platform::LinuxArm64 | Platform::LinuxArmhf | Platform::WindowsArm64)
    }
    /// The platform of the computer this runs on, if there's an engine for it.
    pub fn host() -> Option<Platform> {
        match (std::env::consts::OS, std::env::consts::ARCH) {
            ("linux", "x86_64") => Some(Platform::Linux64),
            ("linux", "x86") => Some(Platform::Linux32),
            ("linux", "aarch64") => Some(Platform::LinuxArm64),
            ("linux", "arm") => Some(Platform::LinuxArmhf),
            ("windows", "x86") => Some(Platform::Windows32),
            ("windows", "x86_64") => Some(Platform::Windows64),
            ("windows", "aarch64") => Some(Platform::WindowsArm64),
            _ => None,
        }
    }
}

/// Where engines, the mkxp-z source and (on Windows) the portable MSYS2 live:
/// <base>/mkxp-z, <base>/mkxp-z_source and <base>/msys2.
#[derive(Clone, Debug)]
pub struct Paths {
    pub engines: PathBuf,
    pub source: PathBuf,
    pub msys2: PathBuf,
}

pub const ENGINES_DIR: &str = "mkxp-z";
pub const SOURCE_DIR: &str = "mkxp-z_source";
pub const MSYS2_DIR: &str = "msys2";

impl Paths {
    pub fn in_dir(base: &Path) -> Paths {
        Paths { engines: base.join(ENGINES_DIR), source: base.join(SOURCE_DIR), msys2: base.join(MSYS2_DIR) }
    }

    /// Default: next to the patcher program (portable). Only if that folder isn't
    /// writable, a per-user folder (~/.local/share/mkxpz-patcher, %LOCALAPPDATA%\\mkxpz-patcher).
    pub fn default_base() -> PathBuf {
        util::exe_dir().filter(|d| util::writable(d)).unwrap_or_else(util::data_dir)
    }

    pub fn default_paths() -> Paths {
        Paths::defaults_for(&Paths::default_base())
    }

    /// MSYS2 (Cygwin, gpg) and the compilers need a local drive: on network or
    /// virtual machine shared folders, locking, links and network paths break
    /// them (corrupt keyring, failing unpacks, GCC not running at all). If the
    /// patcher sits on one, MSYS2 and the source with its build folders go to a
    /// local folder; the finished engines are still put next to the patcher.
    fn defaults_for(base: &Path) -> Paths {
        let mut p = Paths::in_dir(base);
        if cfg!(windows) && util::is_network_dir(base) {
            let local = util::data_dir();
            p.source = local.join(SOURCE_DIR);
            p.msys2 = local.join(MSYS2_DIR);
        }
        p
    }

    /// Make every folder absolute: empty means the default, relative paths are
    /// relative to the default base (the patcher's folder). Meson needs absolute paths.
    pub fn resolved(&self) -> Paths {
        let base = Paths::default_base();
        let def = Paths::defaults_for(&base);
        let fix = |p: &Path, d: &Path| -> PathBuf {
            let t = p.as_os_str().to_string_lossy();
            if t.trim().is_empty() {
                d.to_path_buf()
            } else if p.is_absolute() {
                p.to_path_buf()
            } else {
                base.join(p)
            }
        };
        Paths { engines: fix(&self.engines, &def.engines), source: fix(&self.source, &def.source),
                msys2: fix(&self.msys2, &def.msys2) }
    }
}

// Files of an engine install that aren't copied into the per-platform folder.
const ENGINE_SKIP: [&str; 8] = [ENGINE_STAMP, ".rpgm-patcher-engine", "mkxp.json", "scripts", crate::fonts::CJK_FONT_FILE, "liberation.ttf", "LICENSE.txt", "COPYING"];

/// Written into engines the patcher builds. Engines without the current revision
/// were built from source without the patcher's fixes (fix_source) and are rebuilt.
const ENGINE_STAMP: &str = ".mkxpz-patcher-engine";
const ENGINE_REV: &str = "18";

fn engine_current(dir: &Path) -> bool {
    // also the stamp written under the patcher's old name (rpgm-patcher)
    [ENGINE_STAMP, ".rpgm-patcher-engine"].iter()
        .find_map(|n| std::fs::read_to_string(dir.join(n)).ok())
        .map_or(false, |s| s.trim() == ENGINE_REV)
}

/// A raw meson build dir of the source: current if built after fix_source ran.
fn build_dir_current(p: Platform, dir: &Path, src: &Path) -> bool {
    let fixed = src.join("src/filesystem/filesystemImpl.cpp");
    let mtime = |f: &Path| std::fs::metadata(f).and_then(|m| m.modified()).ok();
    let exe = mtime(&dir.join(p.exe()));
    std::fs::read_to_string(&fixed).map_or(false, |t| t.contains(GAMEFOLDER_FIX))
        && [fixed, src.join("src/display/frameinterp.cpp"), src.join("src/display/graphics.cpp")].iter()
            .all(|f| matches!((exe, mtime(f)), (Some(e), Some(s)) if e >= s))
}

pub struct Engine {
    pub platform: Platform,
    pub dir: PathBuf,
    /// name inside <game>/mkxp-z/<sub>/ -> source path
    pub files: Vec<(String, PathBuf)>,
    pub scripts: Option<PathBuf>,
    pub license: Option<PathBuf>,
}

impl Engine {
    pub fn has_stdlib(&self) -> bool {
        self.files.iter().any(|(n, _)| n == "stdlib")
    }
    pub fn asset(&self, name: &str) -> Option<PathBuf> {
        [self.dir.join(name), self.dir.parent().map(|p| p.join("assets").join(name)).unwrap_or_default()]
            .into_iter().find(|p| p.is_file())
    }
}

fn candidates(p: Platform, paths: &Paths, explicit: Option<&Path>) -> Vec<PathBuf> {
    if let Some(e) = explicit {
        return vec![e.to_path_buf()];
    }
    let mut c = vec![paths.engines.join(p.target())];
    if p == Platform::Linux64 {
        // an existing native meson build dir from earlier work
        c.push(paths.source.join("build"));
    }
    c
}

pub fn locate(p: Platform, paths: &Paths, explicit: Option<&Path>) -> Result<Engine, String> {
    let cands = candidates(p, paths, explicit);
    let mut outdated = None;
    for c in &cands {
        let dir = if c.is_file() { c.parent().map(Path::to_path_buf).unwrap_or_default() } else { c.clone() };
        if dir.join(p.exe()).is_file() {
            // an engine the user picked is taken as it is
            let current = explicit.is_some()
                || if *c == paths.engines.join(p.target()) { engine_current(&dir) } else { build_dir_current(p, &dir, &paths.source) };
            if current {
                return Ok(load_engine(p, &dir));
            }
            outdated.get_or_insert(dir);
        }
    }
    if let Some(d) = outdated {
        return Err(format!("the {} engine in {} was built by an older patcher and needs to be built again", p.label(), d.display()));
    }
    Err(format!("{} engine not found (looked in {})", p.label(),
                cands.iter().map(|c| c.display().to_string()).collect::<Vec<_>>().join(", ")))
}

fn load_engine(p: Platform, dir: &Path) -> Engine {
    let mut files = Vec::new();
    let root;
    if dir.join("stdlib").is_dir() || dir.join("scripts").is_dir() {
        // meson install layout: take everything it installed
        for e in std::fs::read_dir(dir).into_iter().flatten().flatten() {
            let name = e.file_name().to_string_lossy().into_owned();
            if !ENGINE_SKIP.contains(&name.as_str()) {
                files.push((name, e.path()));
            }
        }
        root = dir.to_path_buf();
    } else {
        // raw meson build dir
        files.push((p.exe().to_string(), dir.join(p.exe())));
        let rubies = dir.join("subprojects");
        if let Some(stdlib) = std::fs::read_dir(&rubies).into_iter().flatten().flatten()
            .filter(|e| e.file_name().to_string_lossy().starts_with("ruby-"))
            .filter_map(|e| std::fs::read_dir(e.path().join("dist/lib/ruby")).ok())
            .flat_map(|rd| rd.flatten().map(|e| e.path()))
            .find(|p| p.is_dir() && p.file_name().map_or(false, |n| n.to_string_lossy().ends_with(".0"))) {
            files.push(("stdlib".into(), stdlib));
        }
        root = dir.parent().map(Path::to_path_buf).unwrap_or_default();
    }
    files.sort();
    let scripts = Some(root.join("scripts")).filter(|p| p.is_dir());
    let license = ["LICENSE.txt", "COPYING"].iter().map(|n| root.join(n)).find(|p| p.is_file());
    Engine { platform: p, dir: dir.to_path_buf(), files, scripts, license }
}

// ------------------------------------------------------------------ building
//
// Linux hosts build every target (Windows ones are cross-compiled with MinGW-w64).
// Windows hosts build the Windows targets in a portable MSYS2 that the patcher
// downloads into <base>/msys2 by itself (no MSYS2 installation needed).

pub const MSYS2_URL: &str = "https://repo.msys2.org/distrib/msys2-x86_64-latest.sfx.exe";

/// Can this computer build these engines?
pub fn host_can_build(targets: &[Platform]) -> Result<(), String> {
    if cfg!(windows) {
        if let Some(p) = targets.iter().find(|p| !p.is_windows() || p.is_arm()) {
            return Err(format!("The {} engine can only be built on Linux. Build it there, or copy a built \
                                engine folder into the engines folder.", p.label()));
        }
        return Ok(());
    }
    if !cfg!(target_os = "linux") {
        return Err("Engines can be built on Linux and Windows only.".into());
    }
    if util::in_flatpak() {
        return Err("This runs inside a Flatpak sandbox (e.g. VS Code's terminal), where the system's \
                    compilers aren't visible. Start the patcher from a normal terminal or the desktop.".into());
    }
    Ok(())
}

fn packages(distro: Distro, what: &str) -> &'static str {
    match (distro, what) {
        (Distro::Fedora, "base") => "ruby bison cmake meson ninja-build gcc-c++ git",
        (Distro::Fedora, "linux-x86_64") => "libstdc++-static",
        (Distro::Fedora, "linux-i686") => "glibc-devel.i686 libstdc++-devel.i686 libstdc++-static.i686 libatomic-static.i686",
        (Distro::Fedora, "windows-i686") => "mingw32-gcc-c++ mingw32-winpthreads-static",
        (Distro::Fedora, "windows-x86_64") => "mingw64-gcc-c++ mingw64-winpthreads-static",
        // Fedora's ARM cross compilers come without a C library: build in a container
        (Distro::Fedora, "linux-aarch64") | (Distro::Fedora, "linux-armhf") => "podman",
        (Distro::Debian, "base") => "ruby bison cmake meson ninja-build g++ git",
        (Distro::Debian, "linux-i686") => "g++-multilib",
        (Distro::Debian, "windows-i686") => "g++-mingw-w64-i686-posix",
        (Distro::Debian, "windows-x86_64") => "g++-mingw-w64-x86-64-posix",
        (Distro::Debian, "linux-aarch64") => "g++-aarch64-linux-gnu",
        (Distro::Debian, "linux-armhf") => "g++-arm-linux-gnueabihf",
        (Distro::Arch, "base") => "ruby bison cmake meson ninja gcc git",
        (Distro::Arch, "linux-i686") => "lib32-glibc lib32-gcc-libs (multilib repository)",
        (Distro::Arch, "windows-i686") | (Distro::Arch, "windows-x86_64") => "mingw-w64-gcc",
        (Distro::Arch, "linux-aarch64") => "aarch64-linux-gnu-gcc",
        (Distro::Arch, "linux-armhf") => "podman",
        (_, "windows-aarch64") => "curl tar xz",
        _ => "",
    }
}

fn install_hint(pkgs: &[&str]) -> String {
    let list = pkgs.iter().filter(|s| !s.is_empty()).copied().collect::<Vec<_>>().join(" ");
    match util::distro() {
        Distro::Fedora => format!("sudo dnf install {}", list),
        Distro::Debian => format!("sudo apt install {}", list),
        Distro::Arch => format!("sudo pacman -S {}", list),
        Distro::Other => format!("install the equivalents of: {}", list),
    }
}

/// Try compiling + linking a tiny C++ program the way mkxp-z links for `target` (Linux hosts).
fn toolchain_works(target: &str) -> bool {
    let tmp = std::env::temp_dir().join(format!("mkxpz-patcher-tc-{}-{}", target, std::process::id()));
    let _ = std::fs::create_dir_all(&tmp);
    let src = tmp.join("t.cpp");
    if std::fs::write(&src, "#include <atomic>\n#include <string>\nint main(){std::atomic<int> a{1}; std::string s(\"x\"); return a.load()-1+(int)s.size()-1;}\n").is_err() {
        return false;
    }
    let out = tmp.join("t.out");
    let (cxx, flags): (&str, Vec<&str>) = match target {
        "linux-x86_64" => ("g++", vec!["-static-libstdc++"]),
        "linux-i686" => ("g++", vec!["-m32", "-static-libstdc++", "-latomic"]),
        "windows-i686" => ("i686-w64-mingw32-g++", vec!["-static-libgcc", "-static-libstdc++", "-Wl,-Bstatic", "-lpthread", "-Wl,-Bdynamic"]),
        "windows-x86_64" => ("x86_64-w64-mingw32-g++", vec!["-static-libgcc", "-static-libstdc++", "-Wl,-Bstatic", "-lpthread", "-Wl,-Bdynamic"]),
        "linux-aarch64" => ("aarch64-linux-gnu-g++", vec!["-static-libstdc++"]),
        "linux-armhf" => ("arm-linux-gnueabihf-g++", vec!["-static-libstdc++"]),
        _ => return false,
    };
    let ok = Command::new(cxx).arg(&src).arg("-o").arg(&out).args(&flags)
        .stdout(Stdio::null()).stderr(Stdio::null()).status().map_or(false, |s| s.success());
    let _ = std::fs::remove_dir_all(&tmp);
    ok
}

/// Everything needed to build these targets, or an error saying what to install.
/// (On Windows the portable MSYS2 and its packages are set up during the build.)
pub fn check_build_tools(targets: &[Platform]) -> Result<(), String> {
    host_can_build(targets)?;
    if cfg!(windows) {
        return Ok(());
    }
    let distro = util::distro();
    let mut need: Vec<&str> = Vec::new();
    for tool in ["meson", "ninja", "cmake", "bison", "ruby", "git"] {
        if util::which(tool).is_none() {
            need.push(packages(distro, "base"));
            break;
        }
    }
    let mut broken = Vec::new();
    for p in targets {
        let ok = match linux_arm_route(*p) {
            Some(r) => r.is_ok(),
            None if *p == Platform::WindowsArm64 => llvm_mingw_installed().is_some()
                || (util::which("curl").is_some() && util::which("tar").is_some() && util::which("xz").is_some()),
            None => toolchain_works(p.target()),
        };
        if !ok {
            broken.push(p.label());
            if need.is_empty() {
                need.push(packages(distro, "base"));
            }
            need.push(packages(distro, p.target()));
        }
    }
    if need.is_empty() {
        return Ok(());
    }
    need.dedup();
    Err(format!("Some build tools are missing{}. Install them with:\n  {}",
                if broken.is_empty() { String::new() } else { format!(" (no working compiler for {})", broken.join(", ")) },
                install_hint(&need)))
}

/// What the confirmation before building should mention besides compile time.
pub fn build_preparation_note(paths: &Paths) -> Option<String> {
    let paths = paths.resolved();
    let shared = util::exe_dir().map_or(false, |d| util::is_network_dir(&d));
    (cfg!(windows) && !msys2_root(&paths).join("usr/bin/bash.exe").is_file()).then(|| format!(
        "First a portable MSYS2 (the compilers) is downloaded and set up in\n  {}\n(about 45 MB download, \
         roughly 2 GB once the compilers are installed). Nothing is installed into Windows.{}",
        paths.msys2.display(),
        if shared { format!("\n\nThe patcher's folder is on a network or shared drive, where MSYS2 and the \
                     compilers don't work reliably, so MSYS2 and the mkxp-z source with its build folders go to \
                     {} instead. The finished engines are still put next to the patcher.",
                     util::data_dir().display()) } else { String::new() }))
}

/// Don't flash a console window for every tool when running from the GUI on Windows.
fn quiet(cmd: &mut Command) -> &mut Command {
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x0800_0000); // CREATE_NO_WINDOW
    }
    cmd
}

/// Run a command, sending each output line (stdout and stderr) to `log`.
fn run(cmd: &mut Command, log: &(dyn Fn(String) + Sync)) -> Result<(), String> {
    let shown = format!("{:?}", cmd).replace('"', "");
    log(format!("$ {}", shown));
    let mut child = quiet(cmd).stdout(Stdio::piped()).stderr(Stdio::piped()).stdin(Stdio::null())
        .spawn().map_err(|e| format!("couldn't start {}: {}", shown, e))?;
    let (out, err) = (child.stdout.take().unwrap(), child.stderr.take().unwrap());
    std::thread::scope(|s| {
        s.spawn(|| BufReader::new(err).lines().map_while(Result::ok).for_each(|l| log(l)));
        BufReader::new(out).lines().map_while(Result::ok).for_each(|l| log(l));
    });
    let status = child.wait().map_err(|e| e.to_string())?;
    if status.success() { Ok(()) } else { Err(format!("{} failed ({})", shown, status)) }
}

fn msys2_root(paths: &Paths) -> PathBuf {
    paths.msys2.join("msys64")
}

fn sh_quote(s: &str) -> String {
    format!("'{}'", s.replace('\'', "'\\''"))
}

/// Runs build tools: directly on Linux, inside the portable MSYS2 (MINGW64) on
/// Windows, or in a Debian container (Linux ARM engines on hosts without an
/// ARM cross compiler, see linux_arm_route).
struct Builder {
    msys2: Option<PathBuf>,
    container: Option<Container>,
}

/// podman or docker, the image with the build tools, and the folder shared with
/// it (the mkxp-z source with its build folders, at the same path inside).
struct Container {
    runtime: String,
    image: String,
    share: PathBuf,
}

impl Builder {
    fn new(paths: &Paths) -> Builder {
        Builder { msys2: cfg!(windows).then(|| msys2_root(paths)), container: None }
    }

    fn cmd(&self, program: &str, args: &[String]) -> Command {
        if let Some(c) = &self.container {
            let mut cmd = Command::new(&c.runtime);
            cmd.args(["run", "--rm", "-i"]);
            // files it makes belong to the user, not to root
            #[cfg(unix)]
            {
                use std::os::unix::fs::MetadataExt;
                let (uid, gid) = std::fs::metadata(&c.share).map(|m| (m.uid(), m.gid())).unwrap_or((0, 0));
                if c.runtime.ends_with("podman") {
                    cmd.arg("--userns=keep-id");
                }
                cmd.arg("--user").arg(format!("{}:{}", uid, gid));
            }
            let share = c.share.display().to_string();
            // ":z": allowed through SELinux (Fedora)
            cmd.arg("-v").arg(format!("{}:{}:z", share, share)).arg("-w").arg(&share)
                .args(["-e", "HOME=/tmp", "-e", "CCACHE_DISABLE=1"]).arg(&c.image).arg(program).args(args);
            return cmd;
        }
        match &self.msys2 {
            None => {
                let mut c = Command::new(program);
                c.args(args);
                c
            }
            Some(root) => {
                let line = std::iter::once(program.to_string()).chain(args.iter().map(|a| sh_quote(a)))
                    .collect::<Vec<_>>().join(" ");
                let mut c = Command::new(root.join("usr/bin/bash.exe"));
                c.arg("-lc").arg(line).env("MSYSTEM", "MINGW64").env("CHERE_INVOKING", "1");
                c
            }
        }
    }

    fn run(&self, program: &str, args: &[String], log: &(dyn Fn(String) + Sync)) -> Result<(), String> {
        run(&mut self.cmd(program, args), log)
    }
}

fn s(p: &Path) -> String {
    p.display().to_string()
}

/// The packages engine builds need in MSYS2.
fn build_packages(targets: &[Platform]) -> Vec<&'static str> {
    // patch/git: meson applies mkxp-z's fixes to its bundled libraries with them
    let mut pkgs = vec!["bison", "diffutils", "make", "patch", "git", "mingw-w64-x86_64-gcc", "mingw-w64-x86_64-cmake",
                        "mingw-w64-x86_64-meson", "mingw-w64-x86_64-ninja", "mingw-w64-x86_64-ruby",
                        "mingw-w64-x86_64-pkgconf"];
    if targets.contains(&Platform::Windows32) {
        pkgs.push("mingw-w64-i686-gcc"); // only the compiler; the build tools stay 64-bit
    }
    pkgs
}

/// Download, unpack and initialize the portable MSYS2 (Windows hosts), then
/// install `pkgs` (only the missing ones).
pub fn ensure_msys2(paths: &Paths, pkgs: &[&str], log: &(dyn Fn(String) + Sync)) -> Result<(), String> {
    let paths = &paths.resolved();
    let root = msys2_root(paths);
    let bash = root.join("usr/bin/bash.exe");
    std::fs::create_dir_all(&paths.msys2).map_err(|e| e.to_string())?;
    if !bash.is_file() {
        let sfx = paths.msys2.join("msys2-x86_64-latest.sfx.exe");
        if !sfx.is_file() {
            log(format!("Downloading portable MSYS2 from {} ...", MSYS2_URL));
            let tmp = paths.msys2.join("msys2-download.part");
            let curl = run(Command::new("curl.exe").args(["-L", "--fail", "--retry", "3", "-o"]).arg(&tmp).arg(MSYS2_URL), log);
            if curl.is_err() {
                log("curl failed or is missing; trying PowerShell ...".into());
                run(Command::new("powershell").args(["-NoProfile", "-Command"])
                    .arg(format!("$ProgressPreference='SilentlyContinue'; Invoke-WebRequest -Uri '{}' -OutFile '{}'",
                                 MSYS2_URL, tmp.display())), log)?;
            }
            std::fs::rename(&tmp, &sfx).map_err(|e| e.to_string())?;
        }
        log("Unpacking MSYS2 ...".into());
        // 7-Zip self-extractor: -y = no questions, -o<dir> = target (creates <dir>\msys64)
        run(Command::new(&sfx).arg("-y").arg(format!("-o{}", paths.msys2.display())), log)?;
        if !bash.is_file() {
            return Err(format!("MSYS2 unpacked, but {} is missing", bash.display()));
        }
        let _ = std::fs::remove_file(&sfx);
    }
    let b = Builder { msys2: Some(root.clone()), container: None };
    let marker = root.join(".mkxpz-patcher-ready");
    if !marker.is_file() && root.join(".rpgm-patcher-ready").is_file() {
        // set up by the patcher under its old name (rpgm-patcher)
        let _ = std::fs::rename(root.join(".rpgm-patcher-ready"), &marker);
    }
    let update: Vec<String> = ["-Syuu", "--noconfirm", "--disable-download-timeout"].map(String::from).to_vec();
    if !marker.is_file() {
        log("Setting up MSYS2 (first start) ...".into());
        // MSYS2's first login runs its setup scripts (home folder, keyring). They're
        // sourced into the login shell, so one failing script ends the whole setup,
        // including the keyring step - don't rely on it, check and repair below.
        if b.run("true", &[], log).is_err() {
            log("MSYS2's first-start setup reported an error; checking its result ...".into());
        }
        ensure_keyring(&b, &root, log)?;
        log("Updating MSYS2 ...".into());
        clear_pacman_lock(&root, log);
        // A full update can replace MSYS2's own runtime, which needs a second round.
        let _ = b.run("pacman", &update, log);
        clear_pacman_lock(&root, log);
        b.run("pacman", &update, log).map_err(|e| pacman_error(e, &root))?;
        std::fs::write(&marker, b"").map_err(|e| e.to_string())?;
    }
    let mut args: Vec<String> = ["-S", "--needed", "--noconfirm", "--disable-download-timeout"].map(String::from).to_vec();
    args.extend(pkgs.iter().map(|p| p.to_string()));
    log(format!("Installing into MSYS2 (only what's missing): {}", pkgs.join(" ")));
    clear_pacman_lock(&root, log);
    b.run("pacman", &args, log).map_err(|e| pacman_error(e, &root))
}

/// pacman's package keyring, made fresh: a new local key (pacman-key --init) plus
/// MSYS2's packager keys (--populate). Without a working keyring pacman says
/// "missing required signature" for everything. A keyring left by an earlier,
/// interrupted setup can be half-written or corrupt ("Trust DB error", "no
/// secret key"), so it's never reused while MSYS2 isn't fully set up.
fn ensure_keyring(b: &Builder, root: &Path, log: &(dyn Fn(String) + Sync)) -> Result<(), String> {
    let mut last = String::new();
    for attempt in 1..=2 {
        match make_keyring(b, root, log) {
            Ok(()) => return Ok(()),
            Err(e) => {
                log(format!("Setting up pacman's keyring failed (attempt {}): {}", attempt, e));
                last = e;
            }
        }
    }
    Err(format!("pacman's keyring in {} couldn't be set up ({}), so packages can't be verified. If the patcher's \
                 folder is on a network or shared drive, move it to a local folder; an antivirus program blocking \
                 MSYS2's gpg can also cause this.", root.join("etc/pacman.d/gnupg").display(), last))
}

fn make_keyring(b: &Builder, root: &Path, log: &(dyn Fn(String) + Sync)) -> Result<(), String> {
    let gnupg = root.join("etc/pacman.d/gnupg");
    let kill = || { let _ = b.run("gpgconf", &["--kill".into(), "all".into()], log); };
    kill(); // agents still holding the old keyring
    util::remove_path(&gnupg).map_err(|e| format!("couldn't remove the old keyring: {}", e))?;
    log("Creating pacman's keyring ...".into());
    b.run("pacman-key", &["--init".into()], log)?;
    let has_secret = std::fs::read_dir(gnupg.join("private-keys-v1.d")).map_or(false, |mut d| d.next().is_some());
    if !has_secret {
        kill();
        return Err("pacman-key --init made no secret key".into());
    }
    log("Importing the MSYS2 packager keys ...".into());
    let populated = b.run("pacman-key", &["--populate".into(), "msys2".into()], log);
    kill();
    populated?;
    // pacman's own key alone is ~1-2 KB; with MSYS2's keys it's far bigger.
    let size = std::fs::metadata(gnupg.join("pubring.gpg")).map(|m| m.len()).unwrap_or(0);
    if size < 4096 || !gnupg.join("trustdb.gpg").is_file() {
        return Err("the MSYS2 keys didn't end up in the keyring".into());
    }
    Ok(())
}

/// A pacman run that was killed or failed can leave its lock file behind, which
/// makes every later run fail ("unable to lock database"). Only the patcher uses
/// this MSYS2, one command at a time, so a leftover lock is always stale.
fn clear_pacman_lock(root: &Path, log: &(dyn Fn(String) + Sync)) {
    let lock = root.join("var/lib/pacman/db.lck");
    if lock.exists() && std::fs::remove_file(&lock).is_ok() {
        log("Removed a stale pacman lock file.".into());
    }
}

fn pacman_error(e: String, root: &Path) -> String {
    format!("{}\n\nMSYS2's package manager failed (see the log above). If it complains about signatures or \
             keys, delete {} and try again.", e, root.parent().unwrap_or(root).display())
}

/// The 32-bit compiler from MSYS2's mingw32 tree runs inside the MINGW64
/// environment (where meson, Ruby, CMake live). Its helper programs (cc1.exe,
/// cc1plus.exe, as.exe, ld.exe ...) don't have their DLLs next to them and find
/// them via PATH - which there points to mingw64\bin, full of 64-bit DLLs with
/// the same names, so they can't start ("Unable to get gcc pre-processor
/// defines"). Copy each helper's 32-bit DLLs (with their dependencies) next to it:
/// Windows looks there first.
fn fix_mingw32_helper_dlls(root: &Path, log: &(dyn Fn(String) + Sync)) {
    let m32 = root.join("mingw32");
    let dll_dir = m32.join("bin");
    let mut dirs = vec![m32.join("i686-w64-mingw32").join("bin")];
    dirs.extend(std::fs::read_dir(m32.join("lib/gcc/i686-w64-mingw32")).into_iter().flatten().flatten().map(|e| e.path()));
    dirs.extend(std::fs::read_dir(m32.join("libexec/gcc/i686-w64-mingw32")).into_iter().flatten().flatten().map(|e| e.path()));
    let mut copied = 0;
    for dir in dirs.iter().filter(|d| d.is_dir()) {
        let mut todo: Vec<PathBuf> = std::fs::read_dir(dir).into_iter().flatten().flatten().map(|e| e.path())
            .filter(|p| p.extension().map_or(false, |x| x.eq_ignore_ascii_case("exe"))).collect();
        let mut seen = std::collections::HashSet::new();
        while let Some(file) = todo.pop() {
            let Ok(data) = std::fs::read(&file) else { continue };
            for dll in crate::pe::imported_dlls(&data) {
                if !seen.insert(dll.to_lowercase()) {
                    continue;
                }
                let (src, dst) = (dll_dir.join(&dll), dir.join(&dll));
                if src.is_file() {
                    if !dst.is_file() && std::fs::copy(&src, &dst).is_ok() {
                        copied += 1;
                    }
                    todo.push(src); // its own dependencies, too
                }
            }
        }
    }
    if copied > 0 {
        log(format!("Gave the 32-bit compiler's helper programs their own DLLs ({} copied).", copied));
    }
}

/// Get the mkxp-z source (the tested commit) if it isn't there yet: the commit's
/// archive from GitHub (downloaded with curl, which comes with Linux and Windows
/// 10+), unpacked by the patcher itself. Needs neither git, tar nor MSYS2.
pub fn ensure_source(src: &Path, log: &(dyn Fn(String) + Sync)) -> Result<(), String> {
    ensure_source_from(src, None, log)
}

/// Like ensure_source, optionally from an already downloaded archive (offline use).
pub fn ensure_source_from(src: &Path, archive: Option<&Path>, log: &(dyn Fn(String) + Sync)) -> Result<(), String> {
    if src.join("meson.build").is_file() {
        return Ok(());
    }
    if src.exists() && std::fs::read_dir(src).map_or(false, |mut d| d.next().is_some()) {
        return Err(format!("{} exists but doesn't contain the mkxp-z source; empty or remove it", src.display()));
    }
    let parent = src.parent().map(Path::to_path_buf).unwrap_or_else(|| PathBuf::from("."));
    std::fs::create_dir_all(&parent).map_err(|e| e.to_string())?;
    let tmp = parent.join(format!(".{}.download", src.file_name().unwrap_or_default().to_string_lossy()));
    let _ = util::remove_path(&tmp);
    std::fs::create_dir_all(&tmp).map_err(|e| e.to_string())?;
    let archive = match archive {
        Some(a) => a.to_path_buf(),
        None => {
            let url = format!("https://github.com/mkxp-z/mkxp-z/archive/{}.tar.gz", MKXPZ_COMMIT);
            log(format!("Downloading the mkxp-z source ({}) ...", &MKXPZ_COMMIT[..7]));
            let file = tmp.join("mkxp-z.tar.gz");
            let curl = if cfg!(windows) { "curl.exe" } else { "curl" };
            run(Command::new(curl).args(["-fsSL", "--retry", "3", "-o"]).arg(&file).arg(&url), log)
                .map_err(|e| format!("downloading {} failed: {}", url, e))?;
            file
        }
    };
    log(format!("Unpacking {} ...", archive.display()));
    let data = std::fs::read(&archive).map_err(|e| format!("{}: {}", archive.display(), e))?;
    let unpacked = tmp.join("src");
    let n = crate::targz::extract(&data, &unpacked, 1).map_err(|e| format!("unpacking the mkxp-z source failed: {}", e))?;
    if !unpacked.join("meson.build").is_file() {
        return Err("the archive doesn't contain the mkxp-z source".into());
    }
    // Not a git checkout: tell mkxp-z's build which commit this is.
    let gh = unpacked.join("git-hash");
    let mut text = std::fs::read_to_string(&gh).unwrap_or_default();
    text.push_str(&format!("{}\n", MKXPZ_COMMIT));
    std::fs::write(&gh, text).map_err(|e| e.to_string())?;
    let _ = std::fs::remove_dir(src); // an empty leftover
    std::fs::rename(&unpacked, src).map_err(|e| format!("couldn't move the source into place: {}", e))?;
    let _ = util::remove_path(&tmp);
    log(format!("mkxp-z source ready ({} files): {}", n, src.display()));
    Ok(())
}

/// Upstream mkxp-z changes into a relative "gameFolder" and then checks the
/// result against the same relative path, which is now resolved from the new
/// folder: the check fails and the engine quits at start ("Unable to switch into
/// gameFolder"). Our configs use "../.." (engine in mkxp-z/<platform>/), so
/// compare against the absolute target instead.
const GAMEFOLDER_BUG: &str = "    fs::path stdPath(path);\n    fs::current_path(stdPath);\n";
const GAMEFOLDER_FIX: &str = "    // rpgm-patcher: resolve a relative path before switching into it\n    \
    fs::path stdPath;\n    try {\n        stdPath = fs::absolute(fs::path(path));\n        \
    fs::current_path(stdPath);\n    } catch (...) {\n        return false;\n    }\n";

/// Apply the patcher's fixes to the mkxp-z source (idempotent).
pub fn fix_source(src: &Path, log: &(dyn Fn(String) + Sync)) -> Result<(), String> {
    let file = src.join("src/filesystem/filesystemImpl.cpp");
    let text = std::fs::read_to_string(&file).map_err(|e| format!("{}: {}", file.display(), e))?;
    if !text.contains(GAMEFOLDER_FIX) {
        let fixed = fix_gamefolder(&text)
            .ok_or_else(|| format!("{}: unexpected source, can't apply the gameFolder fix", file.display()))?;
        std::fs::write(&file, fixed).map_err(|e| format!("{}: {}", file.display(), e))?;
        log("Fixed mkxp-z's handling of relative \"gameFolder\" paths.".into());
    }
    // The patches build on each other (a later one may change lines an
    // earlier one added), so the ones applied are listed in a file: an
    // earlier patch can't always tell by itself that it's in place.
    let stamp = src.join(PATCH_STAMP);
    if !stamp.is_file() && src.join(OLD_PATCH_STAMP).is_file() {
        let _ = std::fs::rename(src.join(OLD_PATCH_STAMP), &stamp);
    }
    let mut done: Vec<String> = std::fs::read_to_string(&stamp)
        .map(|t| t.lines().map(|l| l.trim().to_string()).filter(|l| !l.is_empty()).collect())
        .unwrap_or_default();
    let keys: Vec<String> = SOURCE_PATCHES.iter().map(|(name, patch)| patch_key(name, patch)).collect();
    if keys.iter().any(|k| !done.contains(k)) {
        // a source changed before this list existed, or with an earlier version
        // of a patch: the patches are applied in order, so if the last one is
        // in place, all are
        if let Some((_, last)) = SOURCE_PATCHES.last() {
            if crate::srcpatch::is_applied(src, last) {
                done = keys.clone();
                write_stamp(&stamp, &done)?;
            }
        }
    }
    for ((name, patch), key) in SOURCE_PATCHES.iter().zip(&keys) {
        if done.contains(key) {
            continue;
        }
        if crate::srcpatch::is_applied(src, patch) {
            done.push(key.clone());
            write_stamp(&stamp, &done)?;
            continue;
        }
        let n = crate::srcpatch::apply(src, patch).map_err(|e| format!("couldn't add {} to the mkxp-z source: {}", name, e))?;
        if n > 0 {
            log(format!("Added {} to the mkxp-z source ({} file{}).", name, n, if n == 1 { "" } else { "s" }));
        }
        done.push(key.clone());
        write_stamp(&stamp, &done)?;
    }
    Ok(())
}

/// Lists the patches applied to a source folder (see fix_source).
const PATCH_STAMP: &str = ".mkxpz-patcher-patches";
/// The same list, written by the patcher under its old name (rpgm-patcher).
const OLD_PATCH_STAMP: &str = ".rpgm-patcher-patches";

/// Name and content hash (FNV-1a), so a changed patch counts as new.
fn patch_key(name: &str, patch: &str) -> String {
    let mut h: u64 = 0xcbf29ce484222325;
    for b in patch.bytes() {
        h ^= b as u64;
        h = h.wrapping_mul(0x100000001b3);
    }
    format!("{} {:016x}", name.replace(' ', "-"), h)
}

fn write_stamp(path: &Path, keys: &[String]) -> Result<(), String> {
    let text: String = keys.iter().map(|k| format!("{}\n", k)).collect();
    std::fs::write(path, text).map_err(|e| format!("{}: {}", path.display(), e))
}

/// Frame interpolation: in-between frames on displays faster than the game's
/// frame rate ("frameInterpolation" in mkxp.json); see the patch.
pub const FRAME_INTERPOLATION_PATCH: &str = include_str!("../assets/mkxpz-patches/frame-interpolation.patch");

/// F12 reset: the scripts run again with the reset exception in $! / $@, as in
/// RGSS, so "alias ... unless $@" guards work (without it: SystemStackError).
pub const RESET_ERRINFO_PATCH: &str = include_str!("../assets/mkxpz-patches/reset-errinfo.patch");

/// F1 window: options (vsync, frame interpolation, fullscreen at startup,
/// music and sound levels) next to the key bindings, saved in the user's
/// mkxp.json. Builds on the frame interpolation patch.
pub const SETTINGS_MENU_PATCH: &str = include_str!("../assets/mkxpz-patches/settings-menu.patch");

/// Window icon from "iconPath" on Windows too (the patcher sets it to the
/// game's icon); upstream uses it only on Linux.
pub const WINDOW_ICON_PATCH: &str = include_str!("../assets/mkxpz-patches/window-icon.patch");

/// Screen shaders: libretro GLSL presets from mkxp-z/shaders, chosen in the
/// F1 menu ("shader" in mkxp.json). Builds on the F1 menu patch.
pub const SCREEN_SHADERS_PATCH: &str = include_str!("../assets/mkxpz-patches/screen-shaders.patch");

/// Frame interpolation at a chosen rate (F1 slider: off, 61 FPS up to the
/// refresh rate of the window's display, unlimited; "frameInterpolation" is
/// now a number, true/false from older configs still work), paced in fixed
/// slots, and the "60/144 FPS" window title.
pub const INTERPOLATION_RATE_PATCH: &str = include_str!("../assets/mkxpz-patches/interpolation-rate.patch");

/// mkxp-z's own files (settings, key bindings) in a folder set by "dataPath"
/// (the game's mkxp-z folder) or MKXPZ_DATA_PATH (AppImages: ~/.config/<title>)
/// instead of the user's data folder; settings saved there before move along.
pub const DATA_PATH_PATCH: &str = include_str!("../assets/mkxpz-patches/data-path.patch");

/// The patcher's additions to mkxp-z, applied in this order before every build.
const SOURCE_PATCHES: [(&str, &str); 7] = [
    ("frame interpolation", FRAME_INTERPOLATION_PATCH),
    ("the F12 reset fix", RESET_ERRINFO_PATCH),
    ("the options in the F1 menu", SETTINGS_MENU_PATCH),
    ("the game's window icon on Windows", WINDOW_ICON_PATCH),
    ("screen shaders", SCREEN_SHADERS_PATCH),
    ("the frame interpolation rate", INTERPOLATION_RATE_PATCH),
    ("the data folder setting", DATA_PATH_PATCH),
];

fn fix_gamefolder(text: &str) -> Option<String> {
    let t = text.replace("\r\n", "\n");
    (t.matches(GAMEFOLDER_BUG).count() == 1).then(|| t.replace(GAMEFOLDER_BUG, GAMEFOLDER_FIX))
}

/// 32-bit Windows: malloc (and so plain operator new) only returns 8-byte
/// aligned memory, but GCC assumes 16 for i686 and then allocates 16-aligned
/// types (e.g. OpenAL's context) with plain new. Their SSE stores (movaps)
/// crash whenever the heap hands out an address that's only 8-aligned: the
/// engine closes at startup at random. With this, such types use aligned new.
const ALIGNED_NEW: &str = ", '-faligned-new=8'";

/// llvm-mingw (clang for Windows, all CPUs): builds the Windows ARM64 engine,
/// as in mkxp-z's own CI. Taken from PATH if installed, else downloaded once.
const LLVM_MINGW_VERSION: &str = "20260224";

fn llvm_mingw_installed() -> Option<PathBuf> {
    util::which("aarch64-w64-mingw32-clang").and_then(|c| c.parent().map(Path::to_path_buf))
}

/// The bin folder of llvm-mingw, downloading and unpacking it next to the
/// engines folder if needed.
fn ensure_llvm_mingw(paths: &Paths, log: &(dyn Fn(String) + Sync)) -> Result<PathBuf, String> {
    if let Some(bin) = llvm_mingw_installed() {
        return Ok(bin);
    }
    let host = if std::env::consts::ARCH == "aarch64" { "aarch64" } else { "x86_64" };
    let name = format!("llvm-mingw-{}-msvcrt-ubuntu-22.04-{}", LLVM_MINGW_VERSION, host);
    let base = paths.engines.parent().map(Path::to_path_buf).unwrap_or_else(|| PathBuf::from("."));
    let dir = base.join("llvm-mingw");
    let bin = dir.join(&name).join("bin");
    if bin.join("aarch64-w64-mingw32-clang").exists() {
        return Ok(bin);
    }
    std::fs::create_dir_all(&dir).map_err(|e| format!("{}: {}", dir.display(), e))?;
    let url = format!("https://github.com/mstorsjo/llvm-mingw/releases/download/{}/{}.tar.xz", LLVM_MINGW_VERSION, name);
    let file = dir.join(format!("{}.tar.xz", name));
    log(format!("Downloading llvm-mingw (the compiler for Windows on ARM, about 100 MB) from {} ...", url));
    run(Command::new("curl").args(["-fL", "--retry", "3", "-o"]).arg(&file).arg(&url), log)
        .map_err(|e| format!("downloading llvm-mingw failed: {}", e))?;
    log("Unpacking llvm-mingw ...".into());
    let unpacked = run(Command::new("tar").arg("-xJf").arg(&file).arg("-C").arg(&dir), log);
    let _ = std::fs::remove_file(&file);
    unpacked?;
    if !bin.join("aarch64-w64-mingw32-clang").exists() {
        return Err(format!("llvm-mingw unpacked, but {} is missing", bin.join("aarch64-w64-mingw32-clang").display()));
    }
    Ok(bin)
}

/// How a Linux ARM engine gets built here.
#[derive(Debug, Clone, Copy, PartialEq)]
enum ArmRoute {
    /// this computer is that ARM CPU: a normal build
    Native,
    /// with the distribution's cross compiler (Debian/Ubuntu: g++-aarch64-linux-gnu ...)
    Cross,
    /// in a Debian container with its cross compilers (podman or docker), like
    /// mkxp-z's own CI; for distributions whose ARM cross compilers come
    /// without a C library (Fedora)
    Container,
}

/// None for platforms other than Linux ARM.
fn linux_arm_route(p: Platform) -> Option<Result<ArmRoute, String>> {
    if !matches!(p, Platform::LinuxArm64 | Platform::LinuxArmhf) {
        return None;
    }
    Some(if Platform::host() == Some(p) {
        Ok(ArmRoute::Native)
    } else if toolchain_works(p.target()) {
        Ok(ArmRoute::Cross)
    } else if container_runtime().is_some() {
        Ok(ArmRoute::Container)
    } else {
        Err(format!("no {} cross compiler and no podman/docker to build in a container", p.label()))
    })
}

fn container_runtime() -> Option<String> {
    ["podman", "docker"].iter().find(|r| util::which(r).is_some()).map(|r| r.to_string())
}

/// The container image with mkxp-z's build tools and the ARM cross
/// compilers, built once (Debian trixie, as mkxp-z's CI).
const BUILD_IMAGE: &str = "localhost/mkxpz-patcher-build:trixie-1";
const BUILD_IMAGE_DOCKERFILE: &str = "FROM docker.io/library/debian:trixie\n\
RUN apt-get update && apt-get install -y --no-install-recommends bison ca-certificates cmake diffutils git make \
meson ninja-build patch pkgconf python3 ruby gcc g++ gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf && rm -rf /var/lib/apt/lists/*\n";

fn ensure_build_image(runtime: &str, log: &(dyn Fn(String) + Sync)) -> Result<(), String> {
    let have = Command::new(runtime).args(["image", "inspect", BUILD_IMAGE]).stdout(Stdio::null()).stderr(Stdio::null())
        .status().map_or(false, |s| s.success());
    if have {
        return Ok(());
    }
    log(format!("Preparing a Debian container with the ARM cross compilers ({}, once) ...", runtime));
    let mut child = Command::new(runtime).args(["build", "-t", BUILD_IMAGE, "-"])
        .stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::piped()).spawn()
        .map_err(|e| format!("couldn't start {}: {}", runtime, e))?;
    {
        use std::io::Write;
        let mut stdin = child.stdin.take().unwrap();
        stdin.write_all(BUILD_IMAGE_DOCKERFILE.as_bytes()).map_err(|e| e.to_string())?;
    }
    let (out, err) = (child.stdout.take().unwrap(), child.stderr.take().unwrap());
    std::thread::scope(|s| {
        s.spawn(|| BufReader::new(err).lines().map_while(Result::ok).for_each(|l| log(l)));
        BufReader::new(out).lines().map_while(Result::ok).for_each(|l| log(l));
    });
    let st = child.wait().map_err(|e| e.to_string())?;
    if st.success() {
        Ok(())
    } else {
        Err(format!("building the container image failed ({}). With docker, your user may need to be in the \
                     \"docker\" group (or install podman, which needs no extra rights).", st))
    }
}

/// `no_pc`: an empty folder. Cross builds from Linux must not see the build
/// machine's .pc files (e.g. OpenAL would pick up the system's PipeWire headers);
/// every library then comes from mkxp-z's subprojects, like in its own CI.
#[cfg(test)]
fn cross_file(p: Platform, msys2: Option<&Path>, no_pc: &Path) -> Option<String> {
    cross_file_for(p, msys2, no_pc, None, false)
}

/// `llvm`: llvm-mingw's bin folder (Windows ARM64). `in_container`: the
/// build runs in the build container (its compilers, no ccache).
fn cross_file_for(p: Platform, msys2: Option<&Path>, no_pc: &Path, llvm: Option<&Path>, in_container: bool) -> Option<String> {
    let ccache = if msys2.is_none() && !in_container && util::which("ccache").is_some() { "'ccache', " } else { "" };
    // 32-bit x86: GCC defaults to x87 math with excess precision (FLT_EVAL_METHOD 2).
    // In ISO C++ mode (ANGLE uses -std=c++20) that breaks ANGLE's rounding
    // static_asserts, and mkxp-z's configure check misses it because it compiles
    // as gnu++14. Use SSE2 math like clang does, and tell ANGLE to use std::round.
    let sse = "'-msse2', '-mfpmath=sse', '-DMKXPZ_ANGLE_NO_OPTIMIZED_ROUND'";
    let win = |tool: &dyn Fn(&str) -> String, family: &str, cpu: &str, extra: &str, props: &str| format!(
        "[binaries]\nc = [{cc}'{gcc}', '-D_WIN32_WINNT=0x0601'{x}]\ncpp = [{cc}'{gxx}', '-D_WIN32_WINNT=0x0601'{x}{xx}]\n\
         windres = ['{windres}', '-D_WIN32_WINNT=0x0601']\nar = '{ar}'\nstrip = '{strip}'\npkg-config = 'pkg-config'\n\
         cmake = 'cmake'\n\n{props}[host_machine]\nsystem = 'windows'\ncpu_family = '{f}'\ncpu = '{c}'\nendian = 'little'\n",
        cc = ccache, gcc = tool("gcc"), gxx = tool("g++"), windres = tool("windres"), ar = tool("ar"),
        strip = tool("strip"), f = family, c = cpu, x = extra, props = props,
        xx = if family == "x86" { ALIGNED_NEW } else { "" });
    // Windows only promises 4-byte stack alignment on 32-bit, but GCC's SSE code
    // assumes 16: without realignment, the first callback from Windows or new
    // thread crashes on a movaps (the engine just "doesn't start").
    let sse_extra = format!(", {}, '-mstackrealign'", sse);
    let isolated = format!("[properties]\npkg_config_libdir = ['{}']\n\n", no_pc.display());
    match (p, msys2) {
        (Platform::Linux64, _) => None,
        // Windows host: 64-bit is a native MINGW64 build
        (Platform::Windows64, Some(_)) => None,
        // Windows host: 32-bit compiler from MSYS2's mingw32 tree, tools from MINGW64
        (Platform::Windows32, Some(root)) => {
            let bin = root.join("mingw32/bin");
            let tool = |t: &str| bin.join(format!("{}.exe", t)).display().to_string().replace('\\', "/");
            let pc = root.join("mingw32/lib/pkgconfig").display().to_string().replace('\\', "/");
            Some(win(&tool, "x86", "i686", &sse_extra,
                     &format!("[properties]\nneeds_exe_wrapper = false\npkg_config_libdir = ['{}']\n\n", pc)))
        }
        (Platform::Windows32, None) => {
            let tool = |t: &str| format!("i686-w64-mingw32-{}", t);
            Some(win(&tool, "x86", "i686", &sse_extra, &isolated))
        }
        (Platform::Windows64, None) => {
            let tool = |t: &str| format!("x86_64-w64-mingw32-{}", t);
            Some(win(&tool, "x86_64", "x86_64", "", &isolated))
        }
        // this computer's own CPU: a normal build
        (Platform::LinuxArm64, _) | (Platform::LinuxArmhf, _) if Platform::host() == Some(p) => None,
        (Platform::LinuxArm64, _) | (Platform::LinuxArmhf, _) => {
            // like mkxp-z's linux/meson-arm64.txt / meson-armv7.txt; every
            // library comes from mkxp-z's subprojects (no ARM system libraries)
            let (gnu, family, cpu) = if p == Platform::LinuxArm64 { ("aarch64-linux-gnu", "aarch64", "aarch64") }
                                     else { ("arm-linux-gnueabihf", "arm", "armv7") };
            Some(format!(
                "[binaries]\nc = [{cc}'{g}-gcc']\ncpp = [{cc}'{g}-g++']\nar = '{g}-ar'\nstrip = '{g}-strip'\n\
                 pkg-config = 'pkg-config'\ncmake = 'cmake'\n\n{iso}[host_machine]\nsystem = 'linux'\n\
                 cpu_family = '{f}'\ncpu = '{c}'\nendian = 'little'\n",
                cc = ccache, g = gnu, f = family, c = cpu, iso = isolated))
        }
        (Platform::WindowsArm64, _) => {
            // like mkxp-z's windows/meson-clang-arm64.txt, with llvm-mingw's tools
            let bin = llvm.map(|b| format!("{}/", b.display())).unwrap_or_default();
            Some(format!(
                "[binaries]\nc = [{cc}'{b}clang', '--target=aarch64-w64-mingw32', '-D_WIN32_WINNT=0x0601']\n\
                 cpp = [{cc}'{b}clang++', '--target=aarch64-w64-mingw32', '-D_WIN32_WINNT=0x0601']\n\
                 windres = ['{b}llvm-windres', '--target=aarch64-w64-mingw32', '-D_WIN32_WINNT=0x0601']\n\
                 ar = '{b}llvm-ar'\nstrip = '{b}llvm-strip'\npkg-config = 'pkg-config'\ncmake = 'cmake'\n\n\
                 {iso}[host_machine]\nsystem = 'windows'\ncpu_family = 'aarch64'\ncpu = 'aarch64'\nendian = 'little'\n",
                cc = ccache, b = bin, iso = isolated))
        }
        (Platform::Linux32, _) => {
            // Only 32-bit .pc files may be found, or meson would pick 64-bit system libs;
            // everything else comes from mkxp-z's subprojects, like in its own CI.
            let pc = match util::distro() {
                Distro::Debian => "'/usr/lib/i386-linux-gnu/pkgconfig', '/usr/share/pkgconfig'",
                Distro::Arch => "'/usr/lib32/pkgconfig', '/usr/share/pkgconfig'",
                _ => "'/usr/lib/pkgconfig', '/usr/share/pkgconfig'",
            };
            Some(format!(
                "[binaries]\nc = [{cc}'gcc', '-m32', {sse}]\ncpp = [{cc}'g++', '-m32', {sse}]\nar = 'gcc-ar'\nstrip = 'strip'\n\
                 pkg-config = 'pkg-config'\ncmake = 'cmake'\n\n[properties]\nneeds_exe_wrapper = false\n\
                 pkg_config_libdir = [{pc}]\n\n\
                 [host_machine]\nsystem = 'linux'\ncpu_family = 'x86'\ncpu = 'i686'\nendian = 'little'\n",
                cc = ccache, pc = pc, sse = sse))
        }
    }
}

/// The source folder a meson build dir was set up for (from meson-info.json).
fn build_dir_source(bdir: &Path) -> Option<String> {
    let text = std::fs::read_to_string(bdir.join("meson-info/meson-info.json")).ok()?;
    let v = crate::json::Json::parse(&text)?;
    v.get("directories")?.get("source")?.as_str().map(String::from)
}

fn same_path(a: &str, b: &Path) -> bool {
    let norm = |p: &Path| {
        let p = p.canonicalize().unwrap_or_else(|_| p.to_path_buf()).display().to_string().replace('\\', "/");
        let p = p.trim_start_matches("//?/").trim_end_matches('/').to_string();
        if cfg!(windows) { p.to_lowercase() } else { p }
    };
    norm(Path::new(a)) == norm(b)
}

/// Copy MinGW/MSYS2 runtime DLLs a Windows engine needs (if any) next to it,
/// so it runs on a clean Windows. System DLLs aren't in these folders, so
/// they're never copied.
fn bundle_runtime_dlls(p: Platform, out: &Path, msys2: Option<&Path>, llvm: Option<&Path>, log: &(dyn Fn(String) + Sync)) {
    let triplet = match p {
        Platform::Windows32 => "i686-w64-mingw32",
        Platform::WindowsArm64 => "aarch64-w64-mingw32",
        _ => "x86_64-w64-mingw32",
    };
    let mut dirs: Vec<PathBuf> = match msys2 {
        Some(root) => vec![root.join(if p == Platform::Windows32 { "mingw32/bin" } else { "mingw64/bin" })],
        None => vec![PathBuf::from(format!("/usr/{}/sys-root/mingw/bin", triplet)), PathBuf::from(format!("/usr/{}/lib", triplet)),
                     PathBuf::from(format!("/usr/{}/bin", triplet))],
    };
    if let Some(bin) = llvm {
        // llvm-mingw: libc++, libunwind, winpthreads
        if let Some(root) = bin.parent() {
            dirs.insert(0, root.join(triplet).join("bin"));
        }
    }
    if msys2.is_none() {
        // Debian puts libgcc/libstdc++ DLLs into the GCC folder
        for base in [format!("/usr/lib/gcc/{}", triplet)] {
            dirs.extend(std::fs::read_dir(base).into_iter().flatten().flatten().map(|e| e.path()));
        }
    }
    let mut todo = vec![out.join(p.exe())];
    let mut seen = std::collections::HashSet::new();
    while let Some(file) = todo.pop() {
        let Ok(data) = std::fs::read(&file) else { continue };
        for dll in crate::pe::imported_dlls(&data) {
            if !seen.insert(dll.to_lowercase()) || out.join(&dll).is_file() {
                continue;
            }
            if let Some(src) = dirs.iter().map(|d| d.join(&dll)).find(|p| p.is_file()) {
                log(format!("Bundling runtime DLL {}", dll));
                if std::fs::copy(&src, out.join(&dll)).is_ok() {
                    todo.push(out.join(&dll));
                }
            }
        }
    }
}

/// Build one engine and install it into <engines>/<target>/. Long-running.
pub fn build(p: Platform, paths: &Paths, log: &(dyn Fn(String) + Sync)) -> Result<PathBuf, String> {
    host_can_build(&[p])?;
    let paths = &paths.resolved();
    ensure_source(&paths.source, log)?;
    fix_source(&paths.source, log)?;
    if cfg!(windows) {
        ensure_msys2(paths, &build_packages(&[p]), log)?;
        if p == Platform::Windows32 {
            fix_mingw32_helper_dlls(&msys2_root(paths), log);
        }
    }
    let mut b = Builder::new(paths);
    let msys2 = b.msys2.clone();
    let llvm = if p == Platform::WindowsArm64 { Some(ensure_llvm_mingw(paths, log)?) } else { None };
    let mut in_container = false;
    if let Some(route) = linux_arm_route(p) {
        let route = route?;
        log(format!("Building the {} engine: {}", p.label(), match route {
            ArmRoute::Native => "natively on this computer",
            ArmRoute::Cross => "with this computer's ARM cross compiler",
            ArmRoute::Container => "in a Debian container with its ARM cross compiler",
        }));
        if route == ArmRoute::Container {
            let rt = container_runtime().ok_or("no podman or docker")?;
            ensure_build_image(&rt, log)?;
            b.container = Some(Container { runtime: rt, image: BUILD_IMAGE.into(), share: paths.source.clone() });
            in_container = true;
        }
    }
    let src = &paths.source;
    let out = paths.engines.join(p.target());
    let stage = src.join(format!("install-{}", p.target()));
    let mut bdir = src.join(format!("build-{}", p.target()));
    // reuse an existing native build dir instead of building everything again
    if p == Platform::Linux64 && !bdir.exists() && src.join("build/build.ninja").is_file() {
        bdir = src.join("build");
    }
    std::fs::create_dir_all(&paths.engines).map_err(|e| e.to_string())?;
    log(format!("=== Building {} engine in {}", p.label(), bdir.display()));

    // A build dir remembers its source folder and cross file. If the folder was
    // moved, or our compiler settings changed, set it up again from scratch.
    let no_pc = src.join("pkgconfig-none");
    if !cfg!(windows) {
        let _ = std::fs::create_dir_all(&no_pc);
    }
    let cross = cross_file_for(p, msys2.as_deref(), &no_pc, llvm.as_deref(), in_container);
    let record = bdir.join(".mkxpz-patcher-cross.txt");
    let old_record = bdir.join(".rpgm-patcher-cross.txt");
    if !record.is_file() && old_record.is_file() {
        // build folder set up by the patcher under its old name (rpgm-patcher)
        let _ = std::fs::rename(&old_record, &record);
    }
    let configured = bdir.join("build.ninja").is_file();
    let moved = configured && build_dir_source(&bdir).map_or(true, |s| !same_path(&s, src));
    let cross_changed = configured && !moved && cross.is_some() && std::fs::read_to_string(&record).ok() != cross;
    if moved {
        log("The build folder belongs to a source folder at another place; starting it over.".into());
        util::remove_path(&bdir).map_err(|e| e.to_string())?;
    } else if !configured && bdir.exists() {
        log("Clearing a build folder left over from an unfinished setup.".into());
        util::remove_path(&bdir).map_err(|e| e.to_string())?;
    }
    if configured && !moved && !cross_changed {
        b.run("meson", &["configure".into(), s(&bdir), "--prefix".into(), s(&stage)], log)?;
    } else {
        let mut args: Vec<String> = vec!["setup".into()];
        if cross_changed {
            log("The build folder was set up with different compiler settings; starting it over.".into());
            args.push("--wipe".into());
        }
        args.extend(["--buildtype", "release", "-Dstrip=true", "--prefix"].map(String::from));
        args.push(s(&stage));
        if let Some(text) = &cross {
            let cf = src.join(format!("cross-{}.txt", p.target()));
            std::fs::write(&cf, text).map_err(|e| e.to_string())?;
            args.push("--cross-file".into());
            args.push(s(&cf));
        }
        args.push(s(&bdir));
        args.push(s(src));
        b.run("meson", &args, log)?;
        if let Some(text) = &cross {
            std::fs::write(&record, text).map_err(|e| e.to_string())?;
        }
    }
    b.run("ninja", &["-C".into(), s(&bdir)], log)?;
    // Install into a staging folder next to the build, then copy the result to
    // the engines folder ourselves: meson never has to write to the engines
    // folder, which may be on a shared drive.
    let _ = util::remove_path(&stage);
    b.run("meson", &["install".into(), "-C".into(), s(&bdir), "--no-rebuild".into(), "--tags".into(), "mkxp-z".into()], log)?;
    if !stage.join(p.exe()).is_file() {
        return Err(format!("build finished, but {} is missing in {}", p.exe(), stage.display()));
    }
    log(format!("Copying the engine to {} ...", out.display()));
    let _ = util::remove_path(&out);
    util::copy_tree(&stage, &out).map_err(|e| format!("couldn't copy the engine to {}: {}", out.display(), e))?;
    if p.is_windows() {
        bundle_runtime_dlls(p, &out, msys2.as_deref(), llvm.as_deref(), log);
    }
    // fonts the patcher may add to games (not part of the install)
    for f in [crate::fonts::CJK_FONT_FILE, "liberation.ttf"] {
        let _ = std::fs::copy(src.join("assets").join(f), out.join(f));
    }
    std::fs::write(out.join(ENGINE_STAMP), format!("{}\n", ENGINE_REV)).map_err(|e| e.to_string())?;
    log(format!("=== {} engine ready: {}", p.label(), out.display()));
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// MSYS2_SYSROOT=<msys2-sysroot.sh dest with mingw32/> cargo test -- --ignored mingw32_dlls
    #[test]
    #[ignore]
    fn mingw32_dlls() {
        let root = PathBuf::from(std::env::var("MSYS2_SYSROOT").unwrap());
        fix_mingw32_helper_dlls(&root, &|l: String| println!("{}", l));
        let cc1 = std::fs::read_dir(root.join("mingw32/lib/gcc/i686-w64-mingw32")).unwrap().flatten().next().unwrap().path();
        for dll in crate::pe::imported_dlls(&std::fs::read(cc1.join("cc1.exe")).unwrap()) {
            if root.join("mingw32/bin").join(&dll).is_file() {
                assert!(cc1.join(&dll).is_file(), "{} missing next to cc1.exe", dll);
            }
        }
    }
    #[test]
    fn paths_are_absolute() {
        let base = Paths::default_base();
        let r = Paths { engines: "".into(), source: "src-here".into(), msys2: "/abs/m".into() }.resolved();
        assert_eq!(r.engines, base.join(ENGINES_DIR));
        assert_eq!(r.source, base.join("src-here"));
        assert_eq!(r.msys2, PathBuf::from("/abs/m"));
        assert!(r.engines.is_absolute() && r.source.is_absolute());
    }
    #[test]
    fn gamefolder_fix() {
        let orig = "bool filesystemImpl::setCurrentDirectory(const char *path) {\n    fs::path stdPath(path);\n    fs::current_path(stdPath);\n    bool ret;\n";
        let fixed = fix_gamefolder(orig).unwrap();
        assert!(fixed.contains("fs::absolute(fs::path(path))") && fixed.ends_with("    bool ret;\n"));
        assert!(fix_gamefolder(&fixed).is_none());
    }
    #[test]
    fn arm_cross_files() {
        let none = Path::new("/s/none");
        if Platform::host() != Some(Platform::LinuxArm64) {
            let t = cross_file_for(Platform::LinuxArm64, None, none, None, true).unwrap();
            assert!(t.contains("'aarch64-linux-gnu-g++'") && t.contains("cpu_family = 'aarch64'") && !t.contains("ccache"));
            assert!(t.contains("pkg_config_libdir = ['/s/none']"));
        }
        let t = cross_file_for(Platform::LinuxArmhf, None, none, None, true).unwrap();
        assert!(t.contains("'arm-linux-gnueabihf-gcc'") && t.contains("cpu = 'armv7'"));
        let t = cross_file_for(Platform::WindowsArm64, None, none, Some(Path::new("/l/bin")), false).unwrap();
        println!("--- windows-aarch64\n{}", t);
        assert!(t.contains("'/l/bin/clang++', '--target=aarch64-w64-mingw32'") && t.contains("system = 'windows'")
                && t.contains("cpu_family = 'aarch64'") && t.contains("/l/bin/llvm-windres"));
    }

    #[test]
    fn platform_names() {
        for p in ALL_PLATFORMS {
            assert_eq!(Platform::from_key(p.key()), Some(p));
        }
        assert_eq!(Platform::WindowsArm64.sub(), "windows");
        assert!(Platform::WindowsArm64.is_windows() && Platform::LinuxArm64.is_arm() && !Platform::Linux64.is_arm());
    }

    #[test]
    fn cross_files() {
        assert!(cross_file(Platform::Linux64, None, Path::new("/s/none")).is_none());
        assert!(cross_file(Platform::Windows64, Some(Path::new("C:/m/msys64")), Path::new("/s/none")).is_none());
        let w = cross_file(Platform::Windows32, Some(Path::new("C:/m/msys64")), Path::new("/s/none")).unwrap();
        println!("--- windows-i686 on a Windows host\n{}", w);
        assert!(w.contains("C:/m/msys64/mingw32/bin/gcc.exe") && w.contains("-mfpmath=sse"));
        for p in [Platform::Linux32, Platform::Windows32, Platform::Windows64] {
            let t = cross_file(p, None, Path::new("/s/none")).unwrap();
            assert_eq!(t.contains("pkg_config_libdir = ['/s/none']"), p != Platform::Linux32);
            println!("--- {}\n{}", p.target(), t);
            assert!(t.contains("[host_machine]"));
            assert_eq!(t.contains("-mfpmath=sse"), p != Platform::Windows64);
            assert_eq!(t.contains("-mstackrealign"), p == Platform::Windows32);
            // C++ only (GCC warns about it for C)
            let cpp_line = t.lines().find(|l| l.starts_with("cpp = ")).unwrap();
            let c_line = t.lines().find(|l| l.starts_with("c = ")).unwrap();
            assert_eq!(cpp_line.contains("-faligned-new=8"), p == Platform::Windows32);
            assert!(!c_line.contains("-faligned-new"));
        }
    }
}
