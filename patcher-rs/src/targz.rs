//! Unpacking .tar.gz archives (the mkxp-z source) without an external tar.
//! Windows' tar.exe (bsdtar) refuses to write below junctions and redirected
//! folders ("Cannot extract through symlink"), so the patcher does it itself.
//! Only regular files and folders are written; links are skipped.

use std::path::{Component, Path, PathBuf};

/// Strip the gzip wrapper and inflate the deflate data inside.
pub fn gunzip(d: &[u8]) -> Result<Vec<u8>, String> {
    if d.len() < 18 || d[0] != 0x1f || d[1] != 0x8b || d[2] != 8 {
        return Err("not a gzip file".into());
    }
    let flags = d[3];
    let mut p = 10;
    if flags & 4 != 0 {
        let xlen = u16::from_le_bytes([d[p], d[p + 1]]) as usize;
        p += 2 + xlen;
    }
    for bit in [8u8, 16] {
        if flags & bit != 0 {
            // zero-terminated file name / comment
            p += d.get(p..).and_then(|r| r.iter().position(|b| *b == 0)).ok_or("bad gzip header")? + 1;
        }
    }
    if flags & 2 != 0 {
        p += 2;
    }
    crate::game::inflate_raw(d.get(p..).ok_or("bad gzip header")?).ok_or_else(|| "damaged gzip data".to_string())
}

fn octal(field: &[u8]) -> u64 {
    if field.first().map_or(false, |b| b & 0x80 != 0) {
        // GNU base-256 for big sizes
        return field[1..].iter().fold(0u64, |n, b| (n << 8) | *b as u64);
    }
    let s: String = field.iter().take_while(|b| **b != 0 && **b != b' ').map(|b| *b as char).collect();
    u64::from_str_radix(s.trim(), 8).unwrap_or(0)
}

fn cstr(field: &[u8]) -> String {
    let end = field.iter().position(|b| *b == 0).unwrap_or(field.len());
    String::from_utf8_lossy(&field[..end]).into_owned()
}

/// "path" from a pax extended header ("<len> key=value\n" records).
fn pax_path(data: &[u8]) -> Option<String> {
    let mut rest = data;
    while !rest.is_empty() {
        let sp = rest.iter().position(|b| *b == b' ')?;
        let len: usize = std::str::from_utf8(&rest[..sp]).ok()?.parse().ok()?;
        let rec = rest.get(sp + 1..len)?;
        let rec = rec.strip_suffix(b"\n").unwrap_or(rec);
        if let Some(v) = rec.strip_prefix(b"path=") {
            return Some(String::from_utf8_lossy(v).into_owned());
        }
        rest = rest.get(len..)?;
    }
    None
}

/// A safe relative path below `dest` after removing `strip` leading components.
fn target(dest: &Path, name: &str, strip: usize) -> Option<PathBuf> {
    let rel: Vec<&str> = name.split('/').filter(|c| !c.is_empty() && *c != ".").collect();
    if rel.len() <= strip {
        return None;
    }
    let mut out = dest.to_path_buf();
    for c in &rel[strip..] {
        let c = Path::new(c);
        // no "..", no absolute parts, no drive letters
        if !matches!(c.components().next(), Some(Component::Normal(_))) || c.components().count() != 1 {
            return None;
        }
        out.push(c);
    }
    Some(out)
}

/// Unpack a .tar.gz into `dest`, dropping the first `strip` path components.
/// Returns the number of files written.
pub fn extract(archive: &[u8], dest: &Path, strip: usize) -> Result<usize, String> {
    let tar = gunzip(archive)?;
    std::fs::create_dir_all(dest).map_err(|e| e.to_string())?;
    let (mut p, mut files) = (0usize, 0usize);
    let mut long_name: Option<String> = None;
    while p + 512 <= tar.len() {
        let h = &tar[p..p + 512];
        if h.iter().all(|b| *b == 0) {
            break; // end of archive
        }
        let size = octal(&h[124..136]) as usize;
        let kind = h[156];
        let data_start = p + 512;
        let data = tar.get(data_start..data_start + size).ok_or("the archive is truncated")?;
        p = data_start + size.div_ceil(512) * 512;
        match kind {
            b'x' => {
                long_name = pax_path(data);
                continue;
            }
            b'L' => {
                long_name = Some(cstr(data));
                continue;
            }
            b'g' => continue, // global header (GitHub puts the commit id there)
            _ => {}
        }
        let name = long_name.take().unwrap_or_else(|| {
            let (prefix, name) = (cstr(&h[345..500]), cstr(&h[0..100]));
            if &h[257..262] == b"ustar" && !prefix.is_empty() { format!("{}/{}", prefix, name) } else { name }
        });
        let Some(path) = target(dest, &name, strip) else { continue };
        match kind {
            b'5' => std::fs::create_dir_all(&path).map_err(|e| format!("{}: {}", path.display(), e))?,
            b'0' | 0 | b'7' => {
                if let Some(parent) = path.parent() {
                    std::fs::create_dir_all(parent).map_err(|e| format!("{}: {}", parent.display(), e))?;
                }
                std::fs::write(&path, data).map_err(|e| format!("{}: {}", path.display(), e))?;
                #[cfg(unix)]
                if octal(&h[100..108]) & 0o111 != 0 {
                    let _ = crate::util::make_executable(&path);
                }
                files += 1;
            }
            _ => {} // links, devices: not needed for the mkxp-z source
        }
    }
    Ok(files)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_unsafe_paths() {
        let d = Path::new("/dest");
        assert_eq!(target(d, "top/a/b.txt", 1), Some(PathBuf::from("/dest/a/b.txt")));
        assert_eq!(target(d, "top/../x", 1), None);
        assert_eq!(target(d, "top", 1), None);
        assert_eq!(target(d, "top//./a", 1), Some(PathBuf::from("/dest/a")));
    }

    #[test]
    fn pax() {
        assert_eq!(pax_path(b"29 path=very/long/name/x.txt\n").as_deref(), Some("very/long/name/x.txt"));
    }
}
