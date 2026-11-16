//! Does the game's font cover its text? (Windows' RGSS silently borrows missing
//! glyphs from system fonts; mkxp-z uses exactly one font, so we must check.)

use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};

pub const CJK_FONT_FILE: &str = "wqymicrohei.ttf";
pub const CJK_FONT_FAMILY: &str = "wenquanyi micro hei";

fn be16(d: &[u8], p: usize) -> Option<usize> {
    d.get(p..p + 2).map(|b| u16::from_be_bytes([b[0], b[1]]) as usize)
}
fn be32(d: &[u8], p: usize) -> Option<usize> {
    d.get(p..p + 4).map(|b| u32::from_be_bytes([b[0], b[1], b[2], b[3]]) as usize)
}

pub struct FontFace {
    /// family names, lowercase (all languages, name IDs 1 and 16)
    pub names: HashSet<String>,
    pub coverage: HashSet<u32>,
}

/// Family names and character coverage of every face in a .ttf/.otf/.ttc file.
pub fn font_info(path: &Path) -> Vec<FontFace> {
    let Ok(d) = std::fs::read(path) else { return vec![] };
    let offsets: Vec<usize> = if d.starts_with(b"ttcf") {
        let n = be32(&d, 8).unwrap_or(0).min(64);
        (0..n).filter_map(|i| be32(&d, 12 + i * 4)).collect()
    } else {
        vec![0]
    };
    offsets.into_iter().filter_map(|base| parse_face(&d, base)).collect()
}

fn parse_face(d: &[u8], base: usize) -> Option<FontFace> {
    let n = be16(d, base + 4)?;
    let mut tables = HashMap::new();
    for i in 0..n {
        let rec = base + 12 + i * 16;
        tables.insert(d.get(rec..rec + 4)?.to_vec(), be32(d, rec + 8)?);
    }
    let mut names = HashSet::new();
    if let Some(&o) = tables.get(&b"name"[..]) {
        let (cnt, so) = (be16(d, o + 2)?, be16(d, o + 4)?);
        for i in 0..cnt {
            let r = o + 6 + i * 12;
            let (pid, nid, len, off) = (be16(d, r)?, be16(d, r + 6)?, be16(d, r + 8)?, be16(d, r + 10)?);
            if nid != 1 && nid != 16 {
                continue;
            }
            let Some(raw) = d.get(o + so + off..o + so + off + len) else { continue };
            let s = if pid == 0 || pid == 3 {
                let u: Vec<u16> = raw.chunks_exact(2).map(|c| u16::from_be_bytes([c[0], c[1]])).collect();
                String::from_utf16_lossy(&u)
            } else {
                raw.iter().map(|b| *b as char).collect()
            };
            names.insert(s.to_lowercase());
        }
    }
    let mut coverage = HashSet::new();
    if let Some(&o) = tables.get(&b"cmap"[..]) {
        for i in 0..be16(d, o + 2)? {
            let s = o + be32(d, o + 8 + i * 8)?;
            match be16(d, s)? {
                4 => {
                    let seg = be16(d, s + 6)? / 2;
                    for j in 0..seg {
                        let (end, start) = (be16(d, s + 14 + j * 2)?, be16(d, s + 16 + seg * 2 + j * 2)?);
                        if start <= end {
                            coverage.extend(start as u32..=end as u32);
                        }
                    }
                }
                12 => {
                    for g in 0..be32(d, s + 12)?.min(1 << 20) {
                        let (a, b) = (be32(d, s + 16 + g * 12)?, be32(d, s + 20 + g * 12)?);
                        if a <= b && b <= 0x10FFFF {
                            coverage.extend(a as u32..=b as u32);
                        }
                    }
                }
                _ => {}
            }
        }
    }
    Some(FontFace { names, coverage })
}

/// RGSS's built-in Font.default_name per version, used when the scripts don't set one.
fn default_fonts(ver: usize) -> Vec<String> {
    let v: &[&str] = match ver {
        1 => &["Arial", "MS PGothic"],
        2 => &["UmePlus Gothic", "MS Gothic", "Courier New"],
        _ => &["VL Gothic"],
    };
    v.iter().map(|s| s.to_string()).collect()
}

/// Font.default_name as the scripts set it (last assignment wins).
pub fn requested_fonts(scripts: Option<&[(String, Vec<u8>)]>, ver: usize) -> Vec<String> {
    let mut found: Option<Vec<String>> = None;
    for (_, src) in scripts.unwrap_or(&[]) {
        let text = String::from_utf8_lossy(src);
        let mut rest: &str = &text;
        while let Some(i) = rest.find("Font.default_name") {
            rest = &rest[i + "Font.default_name".len()..];
            let t = rest.trim_start();
            let Some(t) = t.strip_prefix('=') else { continue };
            if t.starts_with('=') {
                continue; // "==" comparison
            }
            let t = t.trim_start();
            let body = if let Some(b) = t.strip_prefix('[') {
                b.split(']').next().unwrap_or("")
            } else {
                t.lines().next().unwrap_or("")
            };
            let names = quoted_strings(body);
            if !names.is_empty() {
                found = Some(names);
            }
        }
    }
    found.unwrap_or_else(|| default_fonts(ver))
}

fn quoted_strings(s: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut chars = s.chars();
    while let Some(c) = chars.next() {
        if c == '"' || c == '\'' {
            let v: String = chars.by_ref().take_while(|x| *x != c).collect();
            if !v.trim().is_empty() {
                out.push(v);
            }
        } else if c == '#' {
            break; // comment
        }
    }
    out
}

pub struct FontReport {
    pub requested: Vec<String>,
    /// the requested family the game actually ships, if any
    pub used: Option<String>,
    pub missing: Vec<char>,
    /// replacement font that covers most missing characters
    pub fix: Option<PathBuf>,
}

pub fn analyze(game_dir: &Path, scripts: Option<&[(String, Vec<u8>)]>, ver: usize,
               text: &HashSet<char>, cjk_font: Option<&Path>, builtin_font: Option<&Path>) -> FontReport {
    let requested = requested_fonts(scripts, ver);
    let mut shipped: HashMap<String, HashSet<u32>> = HashMap::new();
    let fonts_dir = std::fs::read_dir(game_dir).ok().and_then(|rd| rd.flatten().map(|e| e.path())
        .find(|p| p.is_dir() && p.file_name().map_or(false, |n| n.eq_ignore_ascii_case("fonts"))));
    if let Some(fd) = fonts_dir {
        let mut stack = vec![fd];
        while let Some(d) = stack.pop() {
            for e in std::fs::read_dir(&d).into_iter().flatten().flatten() {
                let p = e.path();
                if p.is_dir() {
                    stack.push(p);
                } else if p.extension().map_or(false, |x| ["ttf", "otf", "ttc"].iter().any(|y| x.eq_ignore_ascii_case(y))) {
                    for face in font_info(&p) {
                        for n in &face.names {
                            shipped.entry(n.clone()).or_default().extend(face.coverage.iter().copied());
                        }
                    }
                }
            }
        }
    }
    let used = requested.iter().find(|n| shipped.contains_key(&n.to_lowercase())).cloned();
    let coverage: HashSet<u32> = match &used {
        Some(n) => shipped[&n.to_lowercase()].clone(),
        // Nothing requested is available: mkxp-z falls back to its built-in Liberation Sans.
        None => builtin_font.map(font_info).and_then(|f| f.into_iter().next()).map(|f| f.coverage)
            .unwrap_or_else(|| (0..0x250).collect()),
    };
    let mut missing: Vec<char> = text.iter().copied().filter(|c| !coverage.contains(&(*c as u32))).collect();
    missing.sort();
    let mut fix = None;
    if !missing.is_empty() {
        if let Some(cjk) = cjk_font {
            if let Some(face) = font_info(cjk).into_iter().next() {
                let fixable = missing.iter().filter(|c| face.coverage.contains(&(**c as u32))).count();
                if fixable > missing.len() / 2 {
                    fix = Some(cjk.to_path_buf());
                }
            }
        }
    }
    FontReport { requested, used, missing, fix }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn parses_default_name() {
        let src = b"# x\nFont.default_name = [\"SimHei\", \"\xe9\xbb\x91\xe4\xbd\x93\", 'Arial'] # c\n".to_vec();
        let s = vec![("Main".to_string(), src)];
        assert_eq!(requested_fonts(Some(&s), 2), vec!["SimHei", "黑体", "Arial"]);
        let s2 = vec![("x".to_string(), b"if Font.default_name == \"a\"".to_vec())];
        assert_eq!(requested_fonts(Some(&s2), 3), vec!["VL Gothic"]);
    }
}
