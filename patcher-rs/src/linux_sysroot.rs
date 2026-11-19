//! Normal Linux x86 engines: built with this computer's compiler, but against
//! an old C library, so they run on Linux systems from 2020 on, not just on
//! ones as new as the build computer. No container is needed (Linux legacy
//! builds in an Ubuntu 20.04 container instead).
//!
//! Ubuntu 20.04's C library and Linux headers (glibc 2.31; checksums pinned
//! below) are downloaded once into <base>/linux-sysroot/<arch>. Small wrapper
//! scripts around this computer's gcc / g++ compile against them and link
//! the C++ library statically (as mkxp-z does). That C++ library was built
//! for this computer's newer glibc; the few functions it needs that glibc 2.31
//! lacks come from a small C file linked in (SHIM).

use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

use crate::engines::{Paths, Platform};
use crate::util;

/// What the engines need (said in the build log).
pub const BASELINE: &str = "glibc 2.31: Ubuntu 20.04, Debian 11, SteamOS 3 and newer";

const GLIBC: &str = "2.31-0ubuntu9.18";
const KERNEL: &str = "5.4.0-216.236";

struct Deb {
    pool: &'static str,
    name: &'static str,
    version: &'static str,
    sha256: &'static str,
}

const AMD64: [Deb; 3] = [
    Deb { pool: "g/glibc", name: "libc6", version: GLIBC,
          sha256: "1b2281aac4935dfea1f89dfc19e445fdbb45303202af60679ccb8bf035f081a0" },
    Deb { pool: "g/glibc", name: "libc6-dev", version: GLIBC,
          sha256: "11029900f2315b93ce487792ae1bf15f0927e5e50880c091e6a440b14d4e164a" },
    Deb { pool: "l/linux", name: "linux-libc-dev", version: KERNEL,
          sha256: "70c9addd36aabd2d031d5ea0fc444f8b193027aef79ff3d8275d25800aed8ede" },
];

const I386: [Deb; 3] = [
    Deb { pool: "g/glibc", name: "libc6", version: GLIBC,
          sha256: "635b0dd37b832ed3852ea1885d7edc56b73a8f13d9fc474a03af88eddbd82974" },
    Deb { pool: "g/glibc", name: "libc6-dev", version: GLIBC,
          sha256: "68def210ef1f7b8fbb1a6bd3ab88309a866e5ea57e74365a01fa32d26bc4c595" },
    Deb { pool: "l/linux", name: "linux-libc-dev", version: KERNEL,
          sha256: "7c06ad304f1543cd1c6f1dca9ca4fad16c39297136cbd7cd6a2c08ed2f321150" },
];

/// Functions this computer's static C++ library (and libgcc) may use that
/// glibc 2.31 doesn't have. Linked into C++ programs only, so configure
/// checks of C code don't find them.
const SHIM: &str = r#"/* Made by the MKXP-Z Patcher (linux_sysroot.rs): functions of newer C
 * libraries that this computer's static C++ library may use, for programs
 * linked against glibc 2.31. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

/* glibc 2.32: 0 = maybe several threads (the C++ library then always locks) */
char __libc_single_threaded = 0;

/* glibc 2.36 */
uint32_t arc4random(void)
{
	uint32_t v = 0;
	if (syscall(SYS_getrandom, &v, sizeof v, 0) != (long) sizeof v) {
		FILE *f = fopen("/dev/urandom", "rb");
		if (f) {
			if (fread(&v, sizeof v, 1, f) != 1)
				v = 0;
			fclose(f);
		}
	}
	return v;
}

/* glibc 2.38: the C23 versions (they also read 0b numbers) */
long __isoc23_strtol(const char *s, char **e, int b) { return strtol(s, e, b); }
long long __isoc23_strtoll(const char *s, char **e, int b) { return strtoll(s, e, b); }
unsigned long __isoc23_strtoul(const char *s, char **e, int b) { return strtoul(s, e, b); }
unsigned long long __isoc23_strtoull(const char *s, char **e, int b) { return strtoull(s, e, b); }
int __isoc23_vsscanf(const char *s, const char *f, va_list a) { return vsscanf(s, f, a); }
int __isoc23_sscanf(const char *s, const char *f, ...)
{
	va_list a;
	va_start(a, f);
	int r = vsscanf(s, f, a);
	va_end(a);
	return r;
}
"#;

fn debian_arch(p: Platform) -> (&'static str, &'static str, &'static [Deb; 3]) {
    if p.base() == Platform::Linux32 { ("i386", "i386-linux-gnu", &I386) } else { ("amd64", "x86_64-linux-gnu", &AMD64) }
}

/// <base>/linux-sysroot, next to the engines folder (like android-sdk).
fn base_dir(paths: &Paths) -> PathBuf {
    let base = paths.engines.parent().map(Path::to_path_buf).unwrap_or_else(|| PathBuf::from("."));
    base.join("linux-sysroot")
}

/// The commands the download needs (checked before building).
pub fn missing_commands() -> Vec<&'static str> {
    ["curl", "tar", "xz", "sha256sum"].into_iter().filter(|c| util::which(c).is_none()).collect()
}

fn sha256_ok(file: &Path, want: &str) -> bool {
    Command::new("sha256sum").arg(file).stdin(Stdio::null()).output()
        .map_or(false, |o| String::from_utf8_lossy(&o.stdout).split_whitespace().next() == Some(want))
}

/// The members of an ar archive (a .deb): name -> bytes.
fn ar_members(data: &[u8]) -> Result<Vec<(String, &[u8])>, String> {
    if !data.starts_with(b"!<arch>\n") {
        return Err("not a .deb file".into());
    }
    let mut out = Vec::new();
    let mut pos = 8;
    while pos + 60 <= data.len() {
        let name = String::from_utf8_lossy(&data[pos..pos + 16]).trim().trim_end_matches('/').to_string();
        let size: usize = String::from_utf8_lossy(&data[pos + 48..pos + 58]).trim().parse()
            .map_err(|_| "broken .deb file")?;
        let start = pos + 60;
        let end = start.checked_add(size).filter(|e| *e <= data.len()).ok_or("broken .deb file")?;
        out.push((name, &data[start..end]));
        pos = end + (size & 1);
    }
    Ok(out)
}

/// Absolute symbolic links (e.g. libm.so -> /lib/.../libm.so.6) point into
/// the folder instead of this computer's own system.
fn fix_links(root: &Path, dir: &Path) -> std::io::Result<()> {
    for e in std::fs::read_dir(dir)? {
        let path = e?.path();
        let meta = std::fs::symlink_metadata(&path)?;
        if meta.file_type().is_symlink() {
            let target = std::fs::read_link(&path)?;
            if target.is_absolute() {
                let inside = root.join(target.strip_prefix("/").unwrap_or(&target));
                let from = path.parent().unwrap_or(root);
                std::fs::remove_file(&path)?;
                #[cfg(unix)]
                std::os::unix::fs::symlink(relative(from, &inside), &path)?;
            }
        } else if meta.is_dir() {
            fix_links(root, &path)?;
        }
    }
    Ok(())
}

/// `to` as a path relative to the folder `from` (both absolute).
fn relative(from: &Path, to: &Path) -> PathBuf {
    let a: Vec<_> = from.components().collect();
    let b: Vec<_> = to.components().collect();
    let same = a.iter().zip(&b).take_while(|(x, y)| x == y).count();
    let mut out = PathBuf::new();
    for _ in same..a.len() {
        out.push("..");
    }
    for c in &b[same..] {
        out.push(c);
    }
    out
}

/// Download and unpack (once) the old C library for this engine's CPU.
fn ensure(p: Platform, paths: &Paths, log: &(dyn Fn(String) + Sync)) -> Result<PathBuf, String> {
    let (arch, _, debs) = debian_arch(p);
    let root = base_dir(paths).join(arch);
    let stamp = root.join(".mkxpz-sysroot");
    let want = format!("glibc {} linux {} {}\n", GLIBC, KERNEL, arch);
    if std::fs::read_to_string(&stamp).ok().as_deref() == Some(want.as_str()) {
        return Ok(root);
    }
    let cmds = missing_commands();
    if !cmds.is_empty() {
        return Err(format!("Downloading the old C library for Linux engines needs: {}", cmds.join(" ")));
    }
    let mirror = std::env::var("UBUNTU_MIRROR").unwrap_or_else(|_| "http://archive.ubuntu.com/ubuntu".into());
    let mirror = mirror.trim_end_matches('/');
    let dl = base_dir(paths).join(format!("dl-{}", arch));
    let _ = util::remove_path(&root);
    std::fs::create_dir_all(&root).map_err(|e| format!("{}: {}", root.display(), e))?;
    std::fs::create_dir_all(&dl).map_err(|e| format!("{}: {}", dl.display(), e))?;
    for d in debs.iter() {
        let file_name = format!("{}_{}_{}.deb", d.name, d.version, arch);
        let file = dl.join(&file_name);
        if !(file.is_file() && sha256_ok(&file, d.sha256)) {
            // (plain http is fine: the checksum decides)
            let url = format!("{}/pool/main/{}/{}", mirror, d.pool, file_name);
            log(format!("Downloading {} ...", url));
            crate::engines::run(Command::new("curl").args(["-fL", "--retry", "3", "-o"]).arg(&file).arg(&url), log)
                .map_err(|e| format!("downloading {} failed: {}", file_name, e))?;
            if !sha256_ok(&file, d.sha256) {
                let _ = std::fs::remove_file(&file);
                return Err(format!("{}: the download doesn't match its checksum; try again later \
                                    (or set UBUNTU_MIRROR to another Ubuntu mirror)", file_name));
            }
        }
        let data = std::fs::read(&file).map_err(|e| format!("{}: {}", file.display(), e))?;
        let members = ar_members(&data).map_err(|e| format!("{}: {}", file_name, e))?;
        let (name, body) = members.iter().find(|(n, _)| n.starts_with("data.tar"))
            .ok_or_else(|| format!("{}: no data.tar inside", file_name))?;
        let tar = dl.join(name);
        std::fs::write(&tar, body).map_err(|e| e.to_string())?;
        let st = Command::new("tar").arg("-xf").arg(&tar).arg("-C").arg(&root).stdin(Stdio::null()).status()
            .map_err(|e| format!("couldn't run tar: {}", e))?;
        let _ = std::fs::remove_file(&tar);
        if !st.success() {
            return Err(format!("unpacking {} failed ({})", file_name, st));
        }
    }
    fix_links(&root, &root).map_err(|e| format!("{}: {}", root.display(), e))?;
    std::fs::write(&stamp, &want).map_err(|e| e.to_string())?;
    let _ = util::remove_path(&dl);
    log(format!("Old C library ready: {} (glibc {})", root.display(), GLIBC));
    Ok(root)
}

/// One argument for sh, quoted.
fn sh(s: &str) -> String {
    if !s.is_empty() && s.bytes().all(|b| b.is_ascii_alphanumeric() || b"/-_.=,+:@".contains(&b)) {
        s.to_string()
    } else {
        format!("'{}'", s.replace('\'', "'\\''"))
    }
}

/// A string for a meson file.
fn meson(s: &str) -> String {
    format!("'{}'", s.replace('\\', "\\\\").replace('\'', "\\'"))
}

/// The libstdc++ include folders of this computer's g++ (in order).
fn cxx_include_dirs(m32: bool) -> Result<Vec<String>, String> {
    let mut cmd = Command::new("g++");
    if m32 {
        cmd.arg("-m32");
    }
    let out = cmd.args(["-xc++", "-E", "-v", "-"]).env("LC_ALL", "C").stdin(Stdio::null()).output()
        .map_err(|e| format!("couldn't run g++: {}", e))?;
    let text = String::from_utf8_lossy(&out.stderr);
    let dirs: Vec<String> = text.lines()
        .skip_while(|l| !l.starts_with("#include <...> search starts here"))
        .skip(1)
        .take_while(|l| !l.starts_with("End of search list"))
        .map(|l| l.trim().to_string())
        .filter(|l| l.contains("/c++/"))
        .collect();
    if dirs.is_empty() {
        return Err("g++ shows no C++ library headers (is g++ installed?)".into());
    }
    Ok(dirs)
}

/// This computer's static C++ library (e.g. libstdc++.a) for the CPU.
fn static_lib(name: &str, m32: bool) -> Option<PathBuf> {
    let mut cmd = Command::new("g++");
    if m32 {
        cmd.arg("-m32");
    }
    let out = cmd.arg(format!("-print-file-name={}", name)).stdin(Stdio::null()).output().ok()?;
    let p = PathBuf::from(String::from_utf8_lossy(&out.stdout).trim());
    (p.is_absolute() && p.is_file()).then(|| p.canonicalize().unwrap_or(p))
}

fn write_if_changed(path: &Path, text: &str) -> Result<bool, String> {
    if std::fs::read_to_string(path).ok().as_deref() == Some(text) {
        return Ok(false);
    }
    std::fs::write(path, text).map_err(|e| format!("{}: {}", path.display(), e))?;
    Ok(true)
}

/// A short hash of the wrappers (in the cross file: a changed toolchain sets
/// the build folder up again).
fn fnv(text: &str) -> String {
    let mut h: u64 = 0xcbf29ce484222325;
    for b in text.bytes() {
        h = (h ^ b as u64).wrapping_mul(0x100000001b3);
    }
    format!("{:016x}", h)
}

/// The meson cross file for a normal Linux x86 engine: sets up the old C
/// library and the compiler wrappers first.
pub fn cross_file(p: Platform, paths: &Paths, no_pc: &Path, log: &(dyn Fn(String) + Sync)) -> Result<String, String> {
    let root = ensure(p, paths, log)?;
    let m32 = p.base() == Platform::Linux32;
    let (_, multiarch, _) = debian_arch(p);
    let tc = base_dir(paths).join(format!("toolchain-{}", p.base().target()));
    let lib = tc.join("lib");
    std::fs::create_dir_all(&lib).map_err(|e| format!("{}: {}", lib.display(), e))?;
    // the static C++ library, alone in a folder (not next to this
    // computer's own C library)
    let missing_cxx = || format!("no static C++ library for {} (Fedora: {}; Debian/Ubuntu: {})", p.label(),
                                 if m32 { "libstdc++-static.i686 glibc-devel.i686" } else { "libstdc++-static" },
                                 if m32 { "g++-multilib" } else { "g++" });
    for (name, needed) in [("libstdc++.a", true), ("libsupc++.a", false)] {
        let link = lib.join(name);
        let _ = std::fs::remove_file(&link);
        match static_lib(name, m32) {
            #[cfg(unix)]
            Some(target) => std::os::unix::fs::symlink(&target, &link).map_err(|e| e.to_string())?,
            _ if needed => return Err(missing_cxx()),
            _ => {}
        }
    }
    let r = root.display().to_string();
    let mut common: Vec<String> = Vec::new();
    if m32 {
        common.push("-m32".into());
    }
    common.extend([format!("--sysroot={}", r), format!("-B{}/usr/lib/{}/", r, multiarch),
                   "-idirafter".into(), format!("{}/usr/include/{}", r, multiarch)]);
    let mut cxx: Vec<String> = Vec::new();
    for d in cxx_include_dirs(m32)? {
        cxx.push("-isystem".into());
        cxx.push(d);
    }
    let link = vec![format!("-L{}/lib/{}", r, multiarch), format!("-L{}/usr/lib/{}", r, multiarch),
                    format!("-L{}", lib.display()),
                    // this computer's libgcc_s was made for its newer C library;
                    // the user's own one is used at run time
                    "-Wl,--allow-shlib-undefined".to_string()];
    // glibc 2.31 still has these in libraries of their own
    let libs = "-lpthread -ldl -lrt";
    let shim_o = tc.join("shim.o");
    let join = |v: &[String]| v.iter().map(|a| sh(a)).collect::<Vec<_>>().join(" ");
    let script = |driver: &str, flags: String, extra: &str| format!(
        "#!/bin/sh\n# Made by the MKXP-Z Patcher (linux_sysroot.rs): {driver} with Ubuntu 20.04's C library\n\
         # ({baseline}), so the programs it links run on older Linux systems too.\n\
         for a in \"$@\"; do case \"$a\" in -c|-S|-E|-M|-MM) exec {driver} {flags} \"$@\";; esac; done\n\
         exec {driver} {flags} {link} \"$@\"{extra} {libs}\n",
        driver = driver, flags = flags, link = join(&link), extra = extra, libs = libs, baseline = BASELINE);
    let cc_text = script("gcc", join(&common), "");
    let cxx_flags = format!("{} {}", join(&cxx), join(&common));
    let cxx_text = script("g++", cxx_flags, &format!(" {}", sh(&shim_o.display().to_string())));
    let cc = tc.join("cc");
    let cxxw = tc.join("c++");
    for (path, text) in [(&cc, &cc_text), (&cxxw, &cxx_text)] {
        write_if_changed(path, text)?;
        util::make_executable(path).map_err(|e| e.to_string())?;
    }
    let shim_c = tc.join("shim.c");
    if write_if_changed(&shim_c, SHIM)? || !shim_o.is_file() {
        let _ = std::fs::remove_file(&shim_o);
        crate::engines::run(Command::new(&cc).args(["-O2", "-fPIC", "-c"]).arg(&shim_c).arg("-o").arg(&shim_o), log)
            .map_err(|e| format!("compiling {} failed: {}", shim_c.display(), e))?;
    }
    // 32-bit x86: SSE2 math (see engines::cross_file_for)
    let sse = if m32 { ", '-msse2', '-mfpmath=sse', '-DMKXPZ_ANGLE_NO_OPTIMIZED_ROUND'" } else { "" };
    let (family, cpu) = if m32 { ("x86", "i686") } else { ("x86_64", "x86_64") };
    Ok(format!(
        "# MKXP-Z Patcher: {label} engine against glibc 2.31 (linux_sysroot.rs); wrappers {hash}\n\
         [binaries]\nc = [{cc}{sse}]\ncpp = [{cxx}{sse}]\nar = 'gcc-ar'\nstrip = 'strip'\npkg-config = 'pkg-config'\n\
         cmake = 'cmake'\n\n[properties]\nneeds_exe_wrapper = false\npkg_config_libdir = [{pc}]\n\n\
         [host_machine]\nsystem = 'linux'\ncpu_family = '{family}'\ncpu = '{cpu}'\nendian = 'little'\n",
        label = p.label(), hash = fnv(&format!("{}{}{}", cc_text, cxx_text, SHIM)),
        cc = meson(&cc.display().to_string()), cxx = meson(&cxxw.display().to_string()), sse = sse,
        pc = meson(&no_pc.display().to_string()), family = family, cpu = cpu))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn paths_and_quoting() {
        assert_eq!(relative(Path::new("/r/usr/lib/x"), Path::new("/r/lib/x/libm.so.6")),
                   PathBuf::from("../../../lib/x/libm.so.6"));
        assert_eq!(sh("-L/a/b"), "-L/a/b");
        assert_eq!(sh("/a b/it's"), "'/a b/it'\\''s'");
        assert_eq!(meson("/a/it's"), "'/a/it\\'s'");
        let mut deb = b"!<arch>\n".to_vec();
        deb.extend(format!("{:<16}{:<32}{:<10}`\n", "data.tar.xz/", "", 3).as_bytes());
        deb.extend(b"abc\n");
        let m = ar_members(&deb).unwrap();
        assert_eq!(m.len(), 1);
        assert_eq!((m[0].0.as_str(), m[0].1), ("data.tar.xz", &b"abc"[..]));
    }
}
