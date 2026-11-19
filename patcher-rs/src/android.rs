//! Android: the tools for building the Android engines, and the APK of a game.
//!
//! The engine is mkxp-z built with Google's NDK as libmain.so (SDL, Ruby and
//! all libraries inside), which SDL's Java code (SDLActivity) loads. A game's
//! APK holds the patched game in assets/game, the engine for each chosen CPU
//! in lib/<cpu>/ and the Java part (classes.dex: SDL's classes and two of
//! ours, see assets/android/java). On the first start, and after an update,
//! the game is copied into the app's own storage, where it can write its
//! saves (ExtractActivity); then mkxp-z runs it (GameActivity).
//!
//! Tools (Linux x86-64 only), downloaded once into <base>/android-sdk with
//! pinned checksums: the NDK (compilers), the SDK build tools (aapt2, d8,
//! zipalign, apksigner), the Android platform (android.jar) and a JDK
//! (javac, keytool and the Java that d8 and apksigner run on).

use crate::engines::{Paths, Platform};
use crate::plan::{Options, Plan};
use crate::util;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

/// Oldest Android the engines run on: 8.0 (Ruby needs nl_langinfo and
/// endpwent, which came with API level 26).
pub const MIN_API: u32 = 26;
/// The Android version APKs are built against (android.jar) and target.
const TARGET_API: u32 = 35;

struct Tool {
    /// what it is, for the log
    what: &'static str,
    url: &'static str,
    /// "sha1:..." (Google's repository lists SHA-1) or "sha256:..."
    sum: &'static str,
    /// folder under android-sdk to unpack into
    into: &'static str,
    /// a file that exists once it's unpacked (relative to android-sdk)
    marker: &'static str,
    size_mb: u32,
}

// From https://dl.google.com/android/repository/repository2-3.xml and the
// Adoptium API (https://api.adoptium.net/v3/assets/latest/21/hotspot).
const NDK_DIR: &str = "android-ndk-r29";
const BUILD_TOOLS_DIR: &str = "build-tools/android-15"; // 35.0.1 unpacks as "android-15"
const PLATFORM_DIR: &str = "platforms/android-35";
const JDK_DIR: &str = "jdk-21.0.13.1+2";
const TOOLS: [Tool; 4] = [
    Tool { what: "Android NDK r29 (the compilers)", url: "https://dl.google.com/android/repository/android-ndk-r29-linux.zip",
           sum: "sha1:87e2bb7e9be5d6a1c6cdf5ec40dd4e0c6d07c30b", into: "", size_mb: 780,
           marker: "android-ndk-r29/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" },
    Tool { what: "Android SDK build tools 35.0.1", url: "https://dl.google.com/android/repository/build-tools_r35.0.1_linux.zip",
           sum: "sha1:e009a9b188cfeb1d2b4c318ab5cb4f1ddc368861", into: "build-tools", size_mb: 60,
           marker: "build-tools/android-15/aapt2" },
    Tool { what: "Android 15 platform (android.jar)", url: "https://dl.google.com/android/repository/platform-35_r02.zip",
           sum: "sha1:0bb560a90a7a2cbd0dd8348224d518b638fe7949", into: "platforms", size_mb: 65,
           marker: "platforms/android-35/android.jar" },
    Tool { what: "Java 21 (Eclipse Temurin JDK)",
           url: "https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.13.1%2B2/OpenJDK21U-jdk_x64_linux_hotspot_21.0.13.1_2.tar.gz",
           sum: "sha256:bb8a93f9ae6e7a693b2b721e85dbeff26ae1a77447985a8d7afcdd9c7cf6222e", into: "", size_mb: 210,
           marker: "jdk-21.0.13.1+2/bin/javac" },
];

/// Where the tools are, once downloaded.
pub struct Tools {
    pub root: PathBuf,
}

impl Tools {
    pub fn ndk(&self) -> PathBuf {
        self.root.join(NDK_DIR)
    }
    pub fn ndk_bin(&self) -> PathBuf {
        self.ndk().join("toolchains/llvm/prebuilt/linux-x86_64/bin")
    }
    fn build_tool(&self, name: &str) -> PathBuf {
        self.root.join(BUILD_TOOLS_DIR).join(name)
    }
    fn android_jar(&self) -> PathBuf {
        self.root.join(PLATFORM_DIR).join("android.jar")
    }
    fn jdk(&self) -> PathBuf {
        self.root.join(JDK_DIR)
    }
    /// A command with the JDK as its Java (d8 and apksigner are Java programs).
    fn java_cmd(&self, program: &Path) -> Command {
        let mut c = Command::new(program);
        let path = std::env::var_os("PATH").unwrap_or_default();
        let mut dirs = vec![self.jdk().join("bin")];
        dirs.extend(std::env::split_paths(&path));
        c.env("JAVA_HOME", self.jdk()).env("PATH", std::env::join_paths(dirs).unwrap_or(path));
        c
    }
}

/// <base>/android-sdk, next to the engines folder (like llvm-mingw).
pub fn tools_dir(paths: &Paths) -> PathBuf {
    let base = paths.engines.parent().map(Path::to_path_buf).unwrap_or_else(|| PathBuf::from("."));
    base.join("android-sdk")
}

/// Can Android engines be built (and APKs made) here?
pub fn host_ok() -> Result<(), String> {
    if !(cfg!(target_os = "linux") && std::env::consts::ARCH == "x86_64") {
        return Err("Android engines and APKs can only be made on 64-bit x86 Linux (Google's NDK).".into());
    }
    Ok(())
}

/// The commands the downloads need (checked before building).
pub fn missing_commands() -> Vec<&'static str> {
    ["curl", "unzip", "tar", "sha1sum", "sha256sum"].into_iter().filter(|c| util::which(c).is_none()).collect()
}

fn checksum_ok(file: &Path, sum: &str) -> Result<bool, String> {
    let (algo, want) = sum.split_once(':').ok_or("bad checksum")?;
    let out = Command::new(format!("{}sum", algo)).arg(file).stdin(Stdio::null()).output()
        .map_err(|e| format!("couldn't run {}sum: {}", algo, e))?;
    Ok(String::from_utf8_lossy(&out.stdout).split_whitespace().next() == Some(want))
}

/// Download (once) whatever of the tools is missing.
pub fn ensure_tools(paths: &Paths, log: &(dyn Fn(String) + Sync)) -> Result<Tools, String> {
    host_ok()?;
    let root = tools_dir(paths);
    let tools = Tools { root: root.clone() };
    let missing: Vec<&Tool> = TOOLS.iter().filter(|t| !root.join(t.marker).exists()).collect();
    if missing.is_empty() {
        return Ok(tools);
    }
    let cmds = missing_commands();
    if !cmds.is_empty() {
        return Err(format!("Downloading the Android tools needs: {}", cmds.join(" ")));
    }
    let dl = root.join("dl");
    std::fs::create_dir_all(&dl).map_err(|e| format!("{}: {}", dl.display(), e))?;
    for t in missing {
        let name = t.url.rsplit('/').next().unwrap_or("download");
        let file = dl.join(name);
        if !(file.is_file() && checksum_ok(&file, t.sum)?) {
            log(format!("Downloading {} (about {} MB) from {} ...", t.what, t.size_mb, t.url));
            crate::engines::run(Command::new("curl").args(["-fL", "--retry", "3", "-o"]).arg(&file).arg(t.url), log)
                .map_err(|e| format!("downloading {} failed: {}", t.what, e))?;
            if !checksum_ok(&file, t.sum)? {
                let _ = std::fs::remove_file(&file);
                return Err(format!("{}: the download doesn't match its checksum ({}); try again later", t.what, t.sum));
            }
        }
        log(format!("Unpacking {} ...", t.what));
        let dest = root.join(t.into);
        std::fs::create_dir_all(&dest).map_err(|e| e.to_string())?;
        let status = if name.ends_with(".zip") {
            Command::new("unzip").args(["-q", "-o"]).arg(&file).arg("-d").arg(&dest).stdin(Stdio::null()).status()
        } else {
            Command::new("tar").arg("-xzf").arg(&file).arg("-C").arg(&dest).stdin(Stdio::null()).status()
        }.map_err(|e| e.to_string())?;
        // unzip returns 1 for warnings (e.g. links it doesn't set up); the
        // marker file decides
        if !root.join(t.marker).exists() {
            return Err(format!("{} unpacked ({}), but {} is missing", t.what, status, t.marker));
        }
        let _ = std::fs::remove_file(&file);
    }
    let _ = std::fs::remove_dir(&dl);
    Ok(tools)
}

/// The NDK's name for a CPU (lib/<abi>/ in APKs, CMake's ANDROID_ABI).
pub fn abi(p: Platform) -> &'static str {
    match p {
        Platform::AndroidX64 => "x86_64",
        _ => "arm64-v8a",
    }
}

/// Meson cross file for an Android engine. CMake subprojects get the NDK
/// through both CMake's own variables and the ones the NDK's toolchain file
/// defines (some projects look for those, e.g. libjxl).
pub fn cross_file(p: Platform, tools: &Tools, no_pc: &Path) -> String {
    let (triple, family) = match p {
        Platform::AndroidX64 => ("x86_64-linux-android", "x86_64"),
        _ => ("aarch64-linux-android", "aarch64"),
    };
    let bin = tools.ndk_bin();
    let b = |n: &str| bin.join(n).display().to_string();
    let ndk = tools.ndk().display().to_string();
    format!("[binaries]\n\
             c = '{cc}'\n\
             cpp = '{cxx}'\n\
             ar = '{ar}'\n\
             strip = '{strip}'\n\
             ranlib = '{ranlib}'\n\
             pkg-config = 'pkg-config'\n\
             cmake = 'cmake'\n\n\
             [properties]\n\
             pkg_config_libdir = ['{pc}']\n\n\
             [host_machine]\n\
             system = 'android'\n\
             cpu_family = '{family}'\n\
             cpu = '{family}'\n\
             endian = 'little'\n\n\
             [cmake]\n\
             CMAKE_ANDROID_NDK = '{ndk}'\n\
             CMAKE_ANDROID_ARCH_ABI = '{abi}'\n\
             CMAKE_SYSTEM_VERSION = '{api}'\n\
             CMAKE_POSITION_INDEPENDENT_CODE = 'ON'\n\
             ANDROID_NDK = '{ndk}'\n\
             ANDROID_ABI = '{abi}'\n\
             ANDROID_PLATFORM = 'android-{api}'\n",
            cc = b(&format!("{}{}-clang", triple, MIN_API)), cxx = b(&format!("{}{}-clang++", triple, MIN_API)),
            ar = b("llvm-ar"), strip = b("llvm-strip"), ranlib = b("llvm-ranlib"), pc = no_pc.display(),
            family = family, ndk = ndk, abi = abi(p), api = MIN_API)
}

/// Meson options for the Android engines: OpenGL ES comes with Android (no
/// ANGLE); everything goes into one shared object (position-independent).
pub fn meson_options() -> Vec<String> {
    vec!["-Dangle=disabled".into(), "-Db_staticpic=true".into()]
}

const EXTRACT_ACTIVITY: &str = include_str!("../assets/android/java/org/mkxpz/patcher/ExtractActivity.java");
const GAME_ACTIVITY: &str = include_str!("../assets/android/java/org/mkxpz/patcher/GameActivity.java");
const TOUCH_CONTROLS: &str = include_str!("../assets/android/java/org/mkxpz/patcher/TouchControls.java");
const SETTINGS_OVERLAY: &str = include_str!("../assets/android/java/org/mkxpz/patcher/SettingsOverlay.java");
pub const DEX: &str = "classes.dex";
/// SDL's Java code (its activity), kept with each Android engine: it has to
/// match the SDL inside libmain.so. Compiled with ours into each APK.
pub const SDL_JAVA: &str = "sdl-java";

/// Copy SDL's Java code from the SDL subproject of the mkxp-z source into an
/// engine (<engine>/sdl-java).
pub fn save_sdl_java(source: &Path, engine: &Path) -> Result<(), String> {
    let sdl = std::fs::read_dir(source.join("subprojects")).map_err(|e| e.to_string())?.flatten()
        .map(|e| e.path())
        .find(|p| p.file_name().map_or(false, |n| n.to_string_lossy().starts_with("SDL2-"))
              && p.join("android-project").is_dir())
        .ok_or("the SDL2 subproject (with its Android Java code) isn't in the mkxp-z source")?;
    let dst = engine.join(SDL_JAVA);
    let _ = util::remove_path(&dst);
    util::copy_tree(&sdl.join("android-project/app/src/main/java"), &dst).map_err(|e| e.to_string())
}

/// SDL's Java code (from the engine) and ours compiled into <out>/classes.dex.
fn build_dex(sdl_java: &Path, tools: &Tools, out: &Path, log: &(dyn Fn(String) + Sync)) -> Result<(), String> {
    let work = out.join(".java-build");
    let _ = util::remove_path(&work);
    let srcdir = work.join("src");
    util::copy_tree(sdl_java, &srcdir).map_err(|e| format!("{}: {}", sdl_java.display(), e))?;
    // the engine is built without SDL's HIDAPI, whose Java side would call into it
    let act = srcdir.join("org/libsdl/app/SDLActivity.java");
    let text = std::fs::read_to_string(&act).map_err(|e| format!("{}: {}", act.display(), e))?;
    let acquire = "mHIDDeviceManager = HIDDeviceManager.acquire(this);";
    if !text.contains(acquire) {
        return Err(format!("{}: unexpected SDLActivity (no HIDDeviceManager.acquire)", act.display()));
    }
    std::fs::write(&act, text.replace(acquire, "mHIDDeviceManager = null; // MKXP-Z Patcher: no HIDAPI in the engine"))
        .map_err(|e| e.to_string())?;
    let ours = srcdir.join("org/mkxpz/patcher");
    std::fs::create_dir_all(&ours).map_err(|e| e.to_string())?;
    std::fs::write(ours.join("ExtractActivity.java"), EXTRACT_ACTIVITY).map_err(|e| e.to_string())?;
    std::fs::write(ours.join("GameActivity.java"), GAME_ACTIVITY).map_err(|e| e.to_string())?;
    std::fs::write(ours.join("TouchControls.java"), TOUCH_CONTROLS).map_err(|e| e.to_string())?;
    std::fs::write(ours.join("SettingsOverlay.java"), SETTINGS_OVERLAY).map_err(|e| e.to_string())?;
    let mut sources = Vec::new();
    collect(&srcdir, "java", &mut sources);
    let classes = work.join("classes");
    std::fs::create_dir_all(&classes).map_err(|e| e.to_string())?;
    let list = work.join("sources.txt");
    std::fs::write(&list, lines(&sources)).map_err(|e| e.to_string())?;
    log("Compiling the Java part (SDL's activity and the patcher's) ...".into());
    let boot = format!("{}:{}", tools.android_jar().display(), tools.build_tool("core-lambda-stubs.jar").display());
    crate::engines::run(Command::new(tools.jdk().join("bin/javac")).args(["-nowarn", "-source", "8", "-target", "8",
        "-Xlint:-options", "-bootclasspath"]).arg(&boot).arg("-d").arg(&classes).arg(format!("@{}", list.display())), log)?;
    let mut class_files = Vec::new();
    collect(&classes, "class", &mut class_files);
    let clist = work.join("classes.txt");
    std::fs::write(&clist, lines(&class_files)).map_err(|e| e.to_string())?;
    let dex = work.join("dex");
    std::fs::create_dir_all(&dex).map_err(|e| e.to_string())?;
    crate::engines::run(tools.java_cmd(&tools.build_tool("d8")).args(["--release", "--min-api", &MIN_API.to_string(), "--lib"])
        .arg(tools.android_jar()).arg("--output").arg(&dex).arg(format!("@{}", clist.display())), log)?;
    std::fs::copy(dex.join(DEX), out.join(DEX)).map_err(|e| format!("{}: {}", DEX, e))?;
    let _ = util::remove_path(&work);
    Ok(())
}

fn collect(dir: &Path, ext: &str, out: &mut Vec<PathBuf>) {
    for e in std::fs::read_dir(dir).into_iter().flatten().flatten() {
        let p = e.path();
        if p.is_dir() {
            collect(&p, ext, out);
        } else if p.extension().map_or(false, |x| x == ext) {
            out.push(p);
        }
    }
    out.sort();
}

fn lines(files: &[PathBuf]) -> String {
    files.iter().map(|f| f.display().to_string() + "\n").collect()
}

// ------------------------------------------------------------------ APKs

/// Where the APK goes: next to the game folder.
pub fn output_path(game_dir: &Path, title: &str) -> PathBuf {
    let parent = game_dir.parent().map(Path::to_path_buf).unwrap_or_else(|| PathBuf::from("."));
    parent.join(format!("{}.apk", crate::appimage::safe_name(title).replace(' ', "_")))
}

/// Android package name from a title: org.mkxpz.game.<letters, digits, _>.
pub fn package_name(title: &str) -> String {
    let mut s: String = title.chars().filter_map(|c| {
        let c = c.to_ascii_lowercase();
        if c.is_ascii_alphanumeric() { Some(c) } else if c == ' ' || c == '-' || c == '_' { Some('_') } else { None }
    }).collect();
    while s.contains("__") {
        s = s.replace("__", "_");
    }
    let s = s.trim_matches('_').to_string();
    let s = if s.is_empty() { "game".to_string() } else { s };
    // each part must start with a letter
    if s.starts_with(|c: char| c.is_ascii_digit()) { format!("org.mkxpz.game.g{}", s) } else { format!("org.mkxpz.game.{}", s) }
}

fn xml_escape(s: &str) -> String {
    s.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;").replace('"', "&quot;").replace('\'', "\\'")
}

fn manifest(package: &str, label: &str, version_code: u32) -> String {
    format!(r#"<?xml version="1.0" encoding="utf-8"?>
<!-- Made by the MKXP-Z Patcher -->
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="{package}"
    android:versionCode="{version_code}"
    android:versionName="{version}"
    android:installLocation="auto">
    <uses-sdk android:minSdkVersion="{min}" android:targetSdkVersion="{target}" />
    <uses-feature android:glEsVersion="0x00020000" />
    <uses-feature android:name="android.hardware.touchscreen" android:required="false" />
    <uses-feature android:name="android.hardware.gamepad" android:required="false" />
    <uses-feature android:name="android.hardware.type.pc" android:required="false" />
    <uses-permission android:name="android.permission.VIBRATE" />
    <application android:label="{label}"
        android:icon="@mipmap/ic_launcher"
        android:allowBackup="true"
        android:extractNativeLibs="true"
        android:hasCode="true"
        android:theme="@android:style/Theme.Black.NoTitleBar.Fullscreen"
        android:hardwareAccelerated="true">
        <activity android:name="org.mkxpz.patcher.ExtractActivity"
            android:exported="true"
            android:screenOrientation="sensorLandscape"
            android:configChanges="orientation|screenSize|keyboardHidden">
            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="android.intent.category.LAUNCHER" />
            </intent-filter>
        </activity>
        <activity android:name="org.mkxpz.patcher.GameActivity"
            android:exported="false"
            android:alwaysRetainTaskState="true"
            android:launchMode="singleInstance"
            android:screenOrientation="sensorLandscape"
            android:configChanges="layoutDirection|locale|orientation|uiMode|screenLayout|screenSize|smallestScreenSize|keyboard|keyboardHidden|navigation"
            android:preferMinimalPostProcessing="true" />
    </application>
</manifest>
"#, package = package, version_code = version_code, version = env!("CARGO_PKG_VERSION"), min = MIN_API,
       target = TARGET_API, label = xml_escape(label))
}

/// The key APKs are signed with: made once, kept next to the tools, so later
/// APKs of a game install as updates over earlier ones (Android only accepts
/// an update signed with the same key).
fn signing_key(tools: &Tools, log: &(dyn Fn(String) + Sync)) -> Result<PathBuf, String> {
    let ks = tools.root.join("apk-signing.keystore");
    if !ks.is_file() {
        log(format!("Making the key APKs are signed with ({}). Keep it: updates of a game must be signed with \
                     the same key.", ks.display()));
        crate::engines::run(Command::new(tools.jdk().join("bin/keytool")).args(["-genkeypair", "-keystore"]).arg(&ks)
            .args(["-storepass", KEY_PASS, "-keypass", KEY_PASS, "-alias", KEY_ALIAS, "-keyalg", "RSA", "-keysize",
                   "3072", "-validity", "20000", "-dname", "CN=MKXP-Z Patcher"]), log)?;
    }
    Ok(ks)
}

const KEY_ALIAS: &str = "mkxpz";
// a local key for sideloaded APKs, not a secret shared with anyone
const KEY_PASS: &str = "mkxpz-patcher";

/// Every file under `dir`, relative, with forward slashes.
fn list_files(dir: &Path, rel: &str, out: &mut Vec<String>) {
    let mut entries: Vec<_> = std::fs::read_dir(dir).into_iter().flatten().flatten().collect();
    entries.sort_by_key(|e| e.file_name());
    for e in entries {
        let name = e.file_name().to_string_lossy().into_owned();
        let r = if rel.is_empty() { name.clone() } else { format!("{}/{}", rel, name) };
        if e.path().is_dir() {
            list_files(&e.path(), &r, out);
        } else {
            out.push(r);
        }
    }
}

/// Make the APK of a game with the Android engines chosen in `opts`, next to
/// the game folder. Engines must be built already. Returns the file made.
pub fn create(game_dir: &Path, opts: &Options, log: &(dyn Fn(String) + Sync)) -> Result<PathBuf, String> {
    let android: Vec<Platform> = opts.platforms.iter().copied().filter(|p| p.is_android()).collect();
    if android.is_empty() {
        return Err("An APK needs an Android engine: choose Android.".into());
    }
    host_ok()?;
    let check = Plan::new(game_dir, &Options { platforms: android.clone(), ..opts.clone() })?;
    if !check.ready() {
        return Err(format!("the {} engine isn't built yet", check.missing_engines[0].label()));
    }
    let tools = ensure_tools(&opts.paths, log)?;
    let out = output_path(game_dir, &check.title);
    let parent = out.parent().unwrap_or(Path::new("."));
    let stage = parent.join(format!(".{}.build", out.file_name().unwrap_or_default().to_string_lossy()));
    let _ = util::remove_path(&stage);
    let result = (|| -> Result<(), String> {
        let io = |e: std::io::Error| e.to_string();
        let copy = stage.join("assets/game");
        log("Copying the game for the APK ...".into());
        let skipped = crate::appimage::copy_game(game_dir, &copy).map_err(|e| format!("copying the game failed: {}", e))?;
        if skipped > 0 {
            log(format!("Left out {} save file(s) and link(s): the APK starts without saves.", skipped));
        }
        // a patch for other systems isn't needed inside the APK
        if copy.join(crate::plan::ENGINE_SUBDIR).is_dir() || crate::patch::read_manifest(&copy).ok().flatten().is_some() {
            let _ = crate::patch::unpatch(&copy, &|_| {});
        }
        let mut plan = Plan::new(&copy, &Options { platforms: android.clone(), ..opts.clone() })?;
        plan.bundle_rtp = true; // the RTP found on this computer goes into the APK
        crate::patch::apply(&mut plan, &|l| log(l))?;
        let shared = copy.join(crate::plan::ENGINE_SUBDIR);
        for e in std::fs::read_dir(&shared).into_iter().flatten().flatten() {
            if crate::patch::is_player_file(&e.file_name().to_string_lossy()) {
                let _ = std::fs::remove_file(e.path());
            }
        }
        let mut files = Vec::new();
        list_files(&copy, "", &mut files);
        let assets = stage.join("assets");
        std::fs::write(assets.join("game.files"), files.join("\n") + "\n").map_err(io)?;
        let stamp = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
        std::fs::write(assets.join("game.stamp"), format!("{}\n", stamp)).map_err(io)?;

        // resources: the icon
        let icon_png = plan.icon_png.clone()
            .or_else(|| crate::pe::icon_from_ico(crate::patch::MKXPZ_ICON).and_then(|i| i.to_png()))
            .ok_or("no icon")?;
        let res = stage.join("res/mipmap");
        std::fs::create_dir_all(&res).map_err(io)?;
        std::fs::write(res.join("ic_launcher.png"), &icon_png).map_err(io)?;
        let flat = stage.join("res.zip");
        crate::engines::run(Command::new(tools.build_tool("aapt2")).arg("compile").arg("--dir").arg(stage.join("res"))
            .arg("-o").arg(&flat), log)?;

        // minutes since 2020: grows with every APK, so a new one installs as an update
        let version_code = (stamp.saturating_sub(1_577_836_800) / 60).max(1) as u32;
        let package = package_name(&plan.title);
        std::fs::write(stage.join("AndroidManifest.xml"), manifest(&package, &plan.title, version_code)).map_err(io)?;
        log(format!("Making the APK ({}, {} files of the game) ...", package, files.len()));
        let base = stage.join("base.apk");
        crate::engines::run(Command::new(tools.build_tool("aapt2")).arg("link").arg("-o").arg(&base)
            .arg("--manifest").arg(stage.join("AndroidManifest.xml")).arg("-I").arg(tools.android_jar())
            .arg("-R").arg(&flat).arg("--auto-add-overlay")
            .args(["--min-sdk-version", &MIN_API.to_string(), "--target-sdk-version", &TARGET_API.to_string()]), log)?;

        // the engines (lib/<cpu>/libmain.so) and the Java part
        let mut sdl_java = None;
        for &p in &android {
            let e = plan.engine(p).ok_or("engine vanished")?;
            let lib = stage.join("lib").join(abi(p));
            std::fs::create_dir_all(&lib).map_err(io)?;
            std::fs::copy(e.dir.join(p.exe()), lib.join(p.exe()))
                .map_err(|err| format!("{}: {}", e.dir.join(p.exe()).display(), err))?;
            if sdl_java.is_none() && e.dir.join(SDL_JAVA).is_dir() {
                sdl_java = Some(e.dir.join(SDL_JAVA));
            }
        }
        let sdl_java = sdl_java.ok_or("the Android engine has no SDL Java code (sdl-java): build it again")?;
        build_dex(&sdl_java, &tools, &stage, log)?;
        let zip_in = |args: &[&str]| -> Result<(), String> {
            let st = Command::new("zip").current_dir(&stage).args(args).stdin(Stdio::null()).status()
                .map_err(|e| format!("couldn't start zip: {}", e))?;
            if st.success() { Ok(()) } else { Err(format!("zip failed ({})", st)) }
        };
        // classes.dex stored, libraries compressed (extractNativeLibs); the
        // game added here too, not by aapt2, which leaves out files starting
        // with "." and folders starting with "_"
        zip_in(&["-q", "-0", "base.apk", DEX])?;
        zip_in(&["-q", "-r", "base.apk", "lib", "assets"])?;
        let aligned = stage.join("aligned.apk");
        crate::engines::run(Command::new(tools.build_tool("zipalign")).args(["-f", "-p", "16"]).arg(&base).arg(&aligned), log)?;
        let ks = signing_key(&tools, log)?;
        log("Signing the APK ...".into());
        crate::engines::run(tools.java_cmd(&tools.build_tool("apksigner")).arg("sign").arg("--ks").arg(&ks)
            .args(["--ks-pass", &format!("pass:{}", KEY_PASS), "--ks-key-alias", KEY_ALIAS, "--out"]).arg(&out)
            .arg(&aligned), log)?;
        let _ = std::fs::remove_file(out.with_extension("apk.idsig"));
        Ok(())
    })();
    let _ = util::remove_path(&stage);
    result?;
    log(format!("APK ready: {} ({} MB). Install it on Android (allow installing apps from this source), \
                 or with: adb install -r \"{}\"", out.display(),
                std::fs::metadata(&out).map(|m| m.len() / 1_000_000).unwrap_or(0), out.display()));
    Ok(out)
}

/// Commands making an APK needs besides the downloaded tools.
pub fn check_apk_tools() -> Result<(), String> {
    host_ok()?;
    let mut need = missing_commands();
    if util::which("zip").is_none() {
        need.push("zip");
    }
    if need.is_empty() {
        return Ok(());
    }
    Err(format!("Making an APK needs: {}. Install them with your package manager (e.g. sudo dnf install {}).",
                need.join(" "), need.iter().map(|c| if c.starts_with("sha") { "coreutils" } else { c }).collect::<Vec<_>>().join(" ")))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names() {
        assert_eq!(package_name("Dazed (English)"), "org.mkxpz.game.dazed_english");
        assert_eq!(package_name("2 Dragons!"), "org.mkxpz.game.g2_dragons");
        assert_eq!(package_name("ドラゴン"), "org.mkxpz.game.game");
        assert_eq!(abi(Platform::AndroidX64), "x86_64");
        assert_eq!(abi(Platform::AndroidArm64), "arm64-v8a");
        let m = manifest("org.mkxpz.game.a", "A & B", 7);
        assert!(m.contains("A &amp; B") && m.contains("versionCode=\"7\"") && m.contains("minSdkVersion=\"26\""));
        let c = cross_file(Platform::AndroidArm64, &Tools { root: PathBuf::from("/t") }, Path::new("/pc"));
        assert!(c.contains("aarch64-linux-android26-clang'") && c.contains("ANDROID_ABI = 'arm64-v8a'")
                && c.contains("system = 'android'"));
    }
}
