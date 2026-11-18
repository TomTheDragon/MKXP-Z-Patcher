//! Applying the patcher's own changes to the mkxp-z source (unified diffs in
//! assets/mkxpz-patches/), without an external `patch` program.
//!
//! Hunks are matched by their text, not their line numbers: each hunk's old
//! text must occur exactly once in its file. A hunk whose new text is already
//! there counts as applied, so applying twice changes nothing. All files are
//! checked before anything is written; on any mismatch nothing changes.

use std::path::Path;

#[derive(Debug)]
struct Hunk {
    old: String,
    new: String,
}

#[derive(Debug)]
struct FilePatch {
    path: String,
    new_file: bool,
    hunks: Vec<Hunk>,
}

fn counts(header: &str) -> Option<(usize, usize, usize)> {
    // "@@ -12,7 +12,8 @@ ..." (a count of 1 may be left out)
    let mut parts = header.trim_start_matches("@@").split_whitespace();
    let range = |s: &str| -> Option<(usize, usize)> {
        let mut it = s[1..].splitn(2, ',');
        let start = it.next()?.parse().ok()?;
        let len = it.next().map_or(Some(1), |n| n.parse().ok())?;
        Some((start, len))
    };
    let (old_start, old_len) = range(parts.next()?)?;
    let (_, new_len) = range(parts.next()?)?;
    Some((old_start, old_len, new_len))
}

fn parse(text: &str) -> Result<Vec<FilePatch>, String> {
    let lines: Vec<&str> = text.split_inclusive('\n').collect();
    let mut files: Vec<FilePatch> = Vec::new();
    let mut i = 0;
    while i < lines.len() {
        let line = lines[i];
        if let Some(rest) = line.strip_prefix("+++ ") {
            let path = rest.trim_end().trim_start_matches("b/").to_string();
            files.push(FilePatch { path, new_file: false, hunks: Vec::new() });
            i += 1;
        } else if line.starts_with("@@") {
            let file = files.last_mut().ok_or("hunk before a file header")?;
            let (old_start, mut old_left, mut new_left) = counts(line).ok_or_else(|| format!("bad hunk header: {}", line.trim()))?;
            if old_start == 0 && old_left == 0 {
                file.new_file = true;
            }
            let (mut old, mut new) = (String::new(), String::new());
            i += 1;
            while (old_left > 0 || new_left > 0) && i < lines.len() {
                let l = lines[i];
                let body = &l[1.min(l.len())..];
                match l.as_bytes().first() {
                    Some(b' ') => { old.push_str(body); new.push_str(body); old_left -= 1; new_left -= 1; }
                    Some(b'-') => { old.push_str(body); old_left -= 1; }
                    Some(b'+') => { new.push_str(body); new_left -= 1; }
                    Some(b'\n') => { old.push('\n'); new.push('\n'); old_left -= 1; new_left -= 1; }
                    Some(b'\\') => {}
                    _ => return Err(format!("{}: damaged hunk", file.path)),
                }
                i += 1;
            }
            if old_left > 0 || new_left > 0 {
                return Err(format!("{}: truncated hunk", file.path));
            }
            file.hunks.push(Hunk { old, new });
        } else {
            i += 1;
        }
    }
    Ok(files)
}

/// Apply a patch below `root`. Returns the number of files changed (0 if it
/// was applied already).
pub fn apply(root: &Path, patch: &str) -> Result<usize, String> {
    let files = parse(patch)?;
    let mut writes: Vec<(std::path::PathBuf, String)> = Vec::new();
    for f in &files {
        let path = root.join(&f.path);
        if f.new_file {
            let content: String = f.hunks.iter().map(|h| h.new.as_str()).collect();
            if std::fs::read_to_string(&path).ok().as_deref() != Some(content.as_str()) {
                writes.push((path, content));
            }
            continue;
        }
        let orig = std::fs::read_to_string(&path).map_err(|e| format!("{}: {}", path.display(), e))?;
        let mut text = orig.replace("\r\n", "\n");
        for (n, h) in f.hunks.iter().enumerate() {
            if text.matches(h.new.as_str()).count() == 1 {
                continue; // applied already
            }
            match text.matches(h.old.as_str()).count() {
                1 => text = text.replacen(h.old.as_str(), h.new.as_str(), 1),
                0 => return Err(format!("{}: change {} doesn't fit this source", f.path, n + 1)),
                _ => return Err(format!("{}: change {} fits more than one place", f.path, n + 1)),
            }
        }
        if text != orig {
            writes.push((path, text));
        }
    }
    for (path, text) in &writes {
        if let Some(dir) = path.parent() {
            std::fs::create_dir_all(dir).map_err(|e| format!("{}: {}", dir.display(), e))?;
        }
        std::fs::write(path, text).map_err(|e| format!("{}: {}", path.display(), e))?;
    }
    Ok(writes.len())
}

/// The files a patch changes (not the ones it creates), relative to the root.
pub fn changed_files(patch: &str) -> Vec<String> {
    parse(patch).map(|f| f.into_iter().filter(|f| !f.new_file).map(|f| f.path).collect()).unwrap_or_default()
}

/// Whether every change of the patch is in place below `root`.
pub fn is_applied(root: &Path, patch: &str) -> bool {
    let files = match parse(patch) {
        Ok(f) => f,
        Err(_) => return false,
    };
    files.iter().all(|f| {
        let text = match std::fs::read_to_string(root.join(&f.path)) {
            Ok(t) => t.replace("\r\n", "\n"),
            Err(_) => return false,
        };
        if f.new_file {
            let content: String = f.hunks.iter().map(|h| h.new.as_str()).collect();
            return text == content;
        }
        f.hunks.iter().all(|h| text.contains(h.new.as_str()))
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    const PATCH: &str = "--- a/x.txt\n+++ b/x.txt\n@@ -1,3 +1,4 @@\n a\n-b\n+B\n+B2\n c\n\
                         --- a/new.txt\n+++ b/new.txt\n@@ -0,0 +1,2 @@\n+hello\n+world\n";

    #[test]
    fn applies_once() {
        let dir = std::env::temp_dir().join(format!("mkxpz-srcpatch-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("x.txt"), "a\nb\nc\nd\n").unwrap();
        assert_eq!(apply(&dir, PATCH).unwrap(), 2);
        assert_eq!(std::fs::read_to_string(dir.join("x.txt")).unwrap(), "a\nB\nB2\nc\nd\n");
        assert_eq!(std::fs::read_to_string(dir.join("new.txt")).unwrap(), "hello\nworld\n");
        assert_eq!(apply(&dir, PATCH).unwrap(), 0);
        assert!(is_applied(&dir, PATCH));
        std::fs::write(dir.join("x.txt"), "a\nzzz\nc\n").unwrap();
        assert!(apply(&dir, PATCH).is_err());
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn bundled_patches_parse() {
        let files = parse(crate::engines::FRAME_INTERPOLATION_PATCH).unwrap();
        assert!(files.iter().any(|f| f.path == "src/display/frameinterp.cpp" && f.new_file));
        assert!(files.iter().all(|f| !f.hunks.is_empty()));
        let files = parse(crate::engines::SETTINGS_MENU_PATCH).unwrap();
        assert!(files.iter().any(|f| f.path == "src/settingsmenu.cpp") && files.iter().all(|f| !f.hunks.is_empty()));
        let files = parse(crate::engines::SCREEN_SHADERS_PATCH).unwrap();
        assert!(files.iter().any(|f| f.path == "src/display/retroshader.cpp" && f.new_file));
        let files = parse(crate::engines::RESET_ERRINFO_PATCH).unwrap();
        assert!(files.len() == 1 && files[0].path == "binding/binding-mri.cpp" && !files[0].hunks.is_empty());
    }
}
