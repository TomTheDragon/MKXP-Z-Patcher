//! Small platform helpers.

use std::path::{Path, PathBuf};

pub fn home_dir() -> PathBuf {
    std::env::var_os(if cfg!(windows) { "USERPROFILE" } else { "HOME" })
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("."))
}

/// Per-user data folder for engines and the mkxp-z source when the patcher
/// isn't inside the project tree.
pub fn data_dir() -> PathBuf {
    let base = if cfg!(windows) {
        std::env::var_os("LOCALAPPDATA").map(PathBuf::from).unwrap_or_else(home_dir)
    } else {
        std::env::var_os("XDG_DATA_HOME").map(PathBuf::from).unwrap_or_else(|| home_dir().join(".local/share"))
    };
    // keep using the folder of the patcher's old name (rpgm-patcher) with the
    // engines and sources built there
    let (new, old) = (base.join("mkxpz-patcher"), base.join("rpgm-patcher"));
    if !new.exists() && old.is_dir() { old } else { new }
}

/// Is this folder on a network drive or a virtual machine's shared folder (also
/// when reached through a link, e.g. a "Shared" folder on the desktop)?
#[cfg(windows)]
pub fn is_network_dir(dir: &Path) -> bool {
    use std::os::windows::ffi::OsStrExt;
    use windows_sys::Win32::Foundation::{CloseHandle, INVALID_HANDLE_VALUE};
    use windows_sys::Win32::Storage::FileSystem::{CreateFileW, GetDriveTypeW, GetFinalPathNameByHandleW,
                                                  FILE_FLAG_BACKUP_SEMANTICS, FILE_SHARE_DELETE, FILE_SHARE_READ,
                                                  FILE_SHARE_WRITE, OPEN_EXISTING};
    let wide: Vec<u16> = dir.as_os_str().encode_wide().chain(Some(0)).collect();
    let mut real = dir.display().to_string();
    unsafe {
        let h = CreateFileW(wide.as_ptr(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            std::ptr::null(), OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0);
        if h != INVALID_HANDLE_VALUE {
            let mut buf = vec![0u16; 32768];
            let n = GetFinalPathNameByHandleW(h, buf.as_mut_ptr(), buf.len() as u32, 0) as usize;
            CloseHandle(h);
            if n > 0 && n < buf.len() {
                real = String::from_utf16_lossy(&buf[..n]);
            }
        }
    }
    if path_is_unc(&real) {
        return true;
    }
    // a mapped network drive letter (X:\...)
    let stripped = real.trim_start_matches("\\\\?\\");
    let b = stripped.as_bytes();
    if b.len() >= 2 && b[1] == b':' {
        let root: Vec<u16> = format!("{}:\\", stripped[..1].to_string()).encode_utf16().chain(Some(0)).collect();
        return unsafe { GetDriveTypeW(root.as_ptr()) } == 4; // DRIVE_REMOTE
    }
    false
}

#[cfg(not(windows))]
pub fn is_network_dir(_: &Path) -> bool {
    false
}

/// "\\server\share\..." or "\\?\UNC\server\share\..." (but not "\\?\C:\...").
pub fn path_is_unc(p: &str) -> bool {
    let p = p.replace('/', "\\");
    if let Some(rest) = p.strip_prefix("\\\\?\\") {
        return rest.len() >= 4 && rest[..4].eq_ignore_ascii_case("UNC\\");
    }
    p.starts_with("\\\\") && !p.starts_with("\\\\.\\")
}

/// Can we create files in this folder?
pub fn writable(dir: &Path) -> bool {
    let probe = dir.join(format!(".mkxpz-patcher-probe-{}", std::process::id()));
    let ok = std::fs::write(&probe, b"").is_ok();
    let _ = std::fs::remove_file(&probe);
    ok
}

pub fn exe_dir() -> Option<PathBuf> {
    std::env::current_exe().ok()?.parent().map(Path::to_path_buf)
}

/// Inside a Flatpak sandbox (e.g. VS Code's terminal) the host's compilers aren't visible.
pub fn in_flatpak() -> bool {
    Path::new("/.flatpak-info").exists()
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Distro {
    Fedora,
    Debian,
    Arch,
    Other,
}

pub fn distro() -> Distro {
    let text = std::fs::read_to_string("/etc/os-release").unwrap_or_default().to_lowercase();
    let ids: String = text.lines()
        .filter(|l| l.starts_with("id=") || l.starts_with("id_like="))
        .collect::<Vec<_>>().join(" ");
    if ids.contains("fedora") || ids.contains("rhel") {
        Distro::Fedora
    } else if ids.contains("debian") || ids.contains("ubuntu") {
        Distro::Debian
    } else if ids.contains("arch") {
        Distro::Arch
    } else {
        Distro::Other
    }
}

pub fn which(cmd: &str) -> Option<PathBuf> {
    let path = std::env::var_os("PATH")?;
    for dir in std::env::split_paths(&path) {
        for name in [cmd.to_string(), format!("{}.exe", cmd)] {
            let p = dir.join(&name);
            if p.is_file() {
                return Some(p);
            }
        }
    }
    None
}

/// Recursively copy a file or directory (symlinks are followed).
pub fn copy_tree(src: &Path, dst: &Path) -> std::io::Result<()> {
    if src.is_dir() {
        std::fs::create_dir_all(dst)?;
        for e in std::fs::read_dir(src)? {
            let e = e?;
            copy_tree(&e.path(), &dst.join(e.file_name()))?;
        }
        Ok(())
    } else {
        std::fs::copy(src, dst).map(|_| ())
    }
}

pub fn remove_path(p: &Path) -> std::io::Result<()> {
    match std::fs::symlink_metadata(p) {
        Ok(m) if m.is_dir() => std::fs::remove_dir_all(p),
        Ok(_) => std::fs::remove_file(p),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(e) => Err(e),
    }
}

#[cfg(unix)]
pub fn make_executable(p: &Path) -> std::io::Result<()> {
    use std::os::unix::fs::PermissionsExt;
    let mut perm = std::fs::metadata(p)?.permissions();
    perm.set_mode(perm.mode() | 0o755);
    std::fs::set_permissions(p, perm)
}

#[cfg(not(unix))]
pub fn make_executable(_: &Path) -> std::io::Result<()> {
    Ok(())
}

/// Turn what people paste into a plain path: file:// URIs from file managers
/// (percent-encoded), surrounding quotes, trailing newlines.
pub fn clean_path(raw: &str) -> String {
    let s = raw.trim().lines().next().unwrap_or("").trim().trim_matches(|c| c == '"' || c == '\'');
    let Some(rest) = s.strip_prefix("file://") else { return s.to_string() };
    let rest = rest.strip_prefix("localhost").unwrap_or(rest);
    let bytes = rest.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%' && i + 2 < bytes.len() {
            if let Ok(b) = u8::from_str_radix(&rest[i + 1..i + 3], 16) {
                out.push(b);
                i += 3;
                continue;
            }
        }
        out.push(bytes[i]);
        i += 1;
    }
    let p = String::from_utf8_lossy(&out).into_owned();
    // file:///C:/Games -> C:/Games on Windows
    if cfg!(windows) { p.trim_start_matches('/').to_string() } else { p }
}

#[cfg(test)]
mod tests {
    use super::{clean_path, path_is_unc};

    #[test]
    fn unc_paths() {
        assert!(path_is_unc(r"\\vboxsvr\Shared\x"));
        assert!(path_is_unc(r"\\?\UNC\vboxsvr\Shared\x"));
        assert!(path_is_unc(r"\\?\unc\host\share"));
        assert!(!path_is_unc(r"\\?\C:\Users\user\Desktop"));
        assert!(!path_is_unc(r"C:\Users\user"));
        assert!(!path_is_unc(r"\\.\pipe\x"));
    }

    #[test]
    fn pasted_paths() {
        assert_eq!(clean_path("  /home/a/My Game  \n"), "/home/a/My Game");
        assert_eq!(clean_path("\"/home/a/My Game\""), "/home/a/My Game");
        assert_eq!(clean_path("'/x/y'"), "/x/y");
        if !cfg!(windows) {
            assert_eq!(clean_path("file:///home/a/Changed%20%5BSteam%5D"), "/home/a/Changed [Steam]");
            assert_eq!(clean_path("file:///home/a/%E5%A4%9C"), "/home/a/夜");
            assert_eq!(clean_path("file:///x/100%"), "/x/100%");
        }
    }
}
