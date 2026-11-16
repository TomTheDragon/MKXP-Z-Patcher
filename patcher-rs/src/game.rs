//! Reading an RPG Maker game folder: Game.ini, scripts, data files and text.

use crate::archive::Archive;
use crate::marshal::{self, Value};
use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};

pub const RGSS_NAMES: [&str; 4] = ["?", "RPG Maker XP (RGSS1)", "RPG Maker VX (RGSS2)", "RPG Maker VX Ace (RGSS3)"];
const SCRIPT_FILES: [&str; 4] = ["", "Scripts.rxdata", "Scripts.rvdata", "Scripts.rvdata2"];
const DATA_EXTS: [&str; 4] = ["", ".rxdata", ".rvdata", ".rvdata2"];
const ARCHIVE_EXTS: [&str; 4] = ["", ".rgssad", ".rgss2a", ".rgss3a"];

/// Decode text from a Windows-era file: UTF-8, else Shift-JIS, GBK or Windows-1252.
/// Uses the system's converters (GLib/iconv on Linux, Windows' code pages on
/// Windows), so no conversion tables are built into the patcher.
pub fn decode_text(raw: &[u8]) -> String {
    let raw = raw.strip_prefix(b"\xEF\xBB\xBF").unwrap_or(raw);
    if let Ok(s) = std::str::from_utf8(raw) {
        return s.to_string();
    }
    for cp in [(932, "SHIFT_JIS"), (936, "GBK"), (1252, "WINDOWS-1252")] {
        if let Some(s) = convert(raw, cp) {
            return s;
        }
    }
    String::from_utf8_lossy(raw).into_owned()
}

#[cfg(all(not(windows), feature = "gtk"))]
fn convert(raw: &[u8], (_, name): (u32, &str)) -> Option<String> {
    let (bytes, _) = gtk::glib::convert(raw, "UTF-8", name).ok()?;
    std::str::from_utf8(&bytes).ok().map(String::from)
}

/// Without GTK: the C library's iconv (glibc, or musl in static builds).
#[cfg(all(not(windows), not(feature = "gtk")))]
fn convert(raw: &[u8], (_, name): (u32, &str)) -> Option<String> {
    use std::ffi::{c_char, c_void, CString};
    extern "C" {
        fn iconv_open(to: *const c_char, from: *const c_char) -> *mut c_void;
        fn iconv(cd: *mut c_void, inbuf: *mut *mut c_char, inleft: *mut usize, outbuf: *mut *mut c_char,
                 outleft: *mut usize) -> usize;
        fn iconv_close(cd: *mut c_void) -> i32;
    }
    let (to, from) = (CString::new("UTF-8").ok()?, CString::new(name).ok()?);
    unsafe {
        let cd = iconv_open(to.as_ptr(), from.as_ptr());
        if cd as isize == -1 {
            return None;
        }
        let mut input = raw.to_vec();
        let mut out = vec![0u8; raw.len() * 4 + 16];
        let (mut ip, mut il) = (input.as_mut_ptr() as *mut c_char, input.len());
        let (mut op, mut ol) = (out.as_mut_ptr() as *mut c_char, out.len());
        let r = iconv(cd, &mut ip, &mut il, &mut op, &mut ol);
        iconv_close(cd);
        if r == usize::MAX || il != 0 {
            return None;
        }
        out.truncate(out.len() - ol);
        String::from_utf8(out).ok()
    }
}

#[cfg(windows)]
fn convert(raw: &[u8], (cp, _): (u32, &str)) -> Option<String> {
    use windows_sys::Win32::Globalization::{MultiByteToWideChar, MB_ERR_INVALID_CHARS};
    if raw.is_empty() || raw.len() > i32::MAX as usize {
        return Some(String::new());
    }
    unsafe {
        let n = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, raw.as_ptr(), raw.len() as i32, std::ptr::null_mut(), 0);
        if n <= 0 {
            return None;
        }
        let mut w = vec![0u16; n as usize];
        MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, raw.as_ptr(), raw.len() as i32, w.as_mut_ptr(), n);
        Some(String::from_utf16_lossy(&w))
    }
}

/// zlib-inflate: with GIO's decompressor (the system's zlib) on Linux; on
/// Windows, where there's no system zlib, with the small built-in miniz_oxide.
#[cfg(all(not(windows), feature = "gtk"))]
pub fn inflate(data: &[u8]) -> Option<Vec<u8>> {
    gio_inflate(data, gtk::gio::ZlibCompressorFormat::Zlib)
}

/// Inflate raw deflate data (no zlib/gzip header).
#[cfg(all(not(windows), feature = "gtk"))]
pub fn inflate_raw(data: &[u8]) -> Option<Vec<u8>> {
    gio_inflate(data, gtk::gio::ZlibCompressorFormat::Raw)
}

#[cfg(all(not(windows), feature = "gtk"))]
fn gio_inflate(data: &[u8], format: gtk::gio::ZlibCompressorFormat) -> Option<Vec<u8>> {
    use gtk::gio::prelude::*;
    let dec = gtk::gio::ZlibDecompressor::new(format);
    let (mut out, mut input) = (Vec::with_capacity(data.len() * 4), data);
    let mut buf = vec![0u8; 1 << 16];
    loop {
        let (res, read, written) = dec.convert(input, &mut buf, gtk::gio::ConverterFlags::INPUT_AT_END).ok()?;
        out.extend_from_slice(&buf[..written]);
        input = &input[read..];
        if res == gtk::gio::ConverterResult::Finished {
            return Some(out);
        }
        if read == 0 && written == 0 {
            return None;
        }
    }
}

#[cfg(any(windows, not(feature = "gtk")))]
pub fn inflate(data: &[u8]) -> Option<Vec<u8>> {
    miniz_oxide::inflate::decompress_to_vec_zlib(data).ok()
}

#[cfg(any(windows, not(feature = "gtk")))]
pub fn inflate_raw(data: &[u8]) -> Option<Vec<u8>> {
    miniz_oxide::inflate::decompress_to_vec(data).ok()
}

/// The `[Game]` section of the game's .ini (Game.ini preferred).
pub fn read_game_ini(dir: &Path) -> (Option<PathBuf>, HashMap<String, String>) {
    let mut inis: Vec<PathBuf> = std::fs::read_dir(dir)
        .map(|rd| rd.flatten().map(|e| e.path())
            .filter(|p| p.is_file() && p.extension().map_or(false, |e| e.eq_ignore_ascii_case("ini")))
            .collect())
        .unwrap_or_default();
    inis.sort_by_key(|p| (!p.file_name().map_or(false, |n| n.eq_ignore_ascii_case("game.ini")), p.clone()));
    for ini in inis {
        let Ok(raw) = std::fs::read(&ini) else { continue };
        let text = decode_text(&raw);
        let mut in_game = false;
        let mut found = false;
        let mut map = HashMap::new();
        for line in text.lines() {
            let line = line.trim();
            if line.starts_with('[') && line.ends_with(']') {
                in_game = line[1..line.len() - 1].trim().eq_ignore_ascii_case("game");
                found |= in_game;
            } else if in_game {
                if let Some((k, v)) = line.split_once('=') {
                    map.insert(k.trim().to_string(), v.trim().to_string());
                }
            }
        }
        if found {
            return (Some(ini), map);
        }
    }
    (None, HashMap::new())
}

pub fn find_archive(dir: &Path, ver: usize) -> Option<PathBuf> {
    let ext = ARCHIVE_EXTS.get(ver)?;
    if ext.is_empty() {
        return None;
    }
    let mut hits: Vec<PathBuf> = std::fs::read_dir(dir).ok()?.flatten().map(|e| e.path())
        .filter(|p| p.is_file() && p.file_name().map_or(false, |n| n.to_string_lossy().to_lowercase().ends_with(ext)))
        .collect();
    hits.sort();
    hits.into_iter().next()
}

pub fn detect_rgss(dir: &Path, ini: &HashMap<String, String>) -> usize {
    if let Some(lib) = ini.get("Library") {
        let up = lib.to_uppercase();
        if let Some(i) = up.find("RGSS") {
            if let Some(c) = up[i + 4..].chars().next() {
                if let Some(d) = c.to_digit(10) {
                    if (1..=3).contains(&d) {
                        return d as usize;
                    }
                }
            }
        }
    }
    for ver in [3, 2, 1] {
        if dir.join("Data").join(SCRIPT_FILES[ver]).is_file() || find_archive(dir, ver).is_some() {
            return ver;
        }
    }
    0
}

/// Data files, read from the folder or (transparently) from the encrypted archive.
#[derive(Clone)]
pub struct GameFiles {
    pub dir: PathBuf,
    pub archive: Option<Archive>,
    /// false if there is an archive we couldn't open
    pub readable: bool,
}

impl GameFiles {
    pub fn new(dir: &Path, archive: Option<&Path>) -> GameFiles {
        let (arc, readable) = match archive {
            Some(p) => match Archive::open(p) {
                Ok(a) => (Some(a), true),
                Err(_) => (None, false),
            },
            None => (None, true),
        };
        GameFiles { dir: dir.to_path_buf(), archive: arc, readable }
    }

    /// Data files with this extension, relative to Data/ (subfolders included:
    /// some games keep one set per language, e.g. Data/Data2/Map001.rvdata).
    pub fn data_names(&self, ext: &str) -> Vec<String> {
        let mut names = HashSet::new();
        let data = self.dir.join("Data");
        fn walk(base: &Path, d: &Path, ext: &str, out: &mut HashSet<String>) {
            let Ok(rd) = std::fs::read_dir(d) else { return };
            for e in rd.flatten() {
                let p = e.path();
                if p.is_dir() {
                    walk(base, &p, ext, out);
                } else if p.to_string_lossy().to_lowercase().ends_with(ext) {
                    if let Ok(rel) = p.strip_prefix(base) {
                        out.insert(rel.to_string_lossy().replace('\\', "/"));
                    }
                }
            }
        }
        walk(&data, &data, ext, &mut names);
        if let Some(a) = &self.archive {
            for n in a.names() {
                if let Some(rest) = n.strip_prefix("data/") {
                    if rest.ends_with(ext) {
                        names.insert(rest.to_string());
                    }
                }
            }
        }
        let mut v: Vec<String> = names.into_iter().collect();
        v.sort_by_key(|s| s.to_lowercase());
        v.dedup_by_key(|s| s.to_lowercase());
        v
    }

    pub fn read_data(&self, name: &str) -> Option<Vec<u8>> {
        let p = self.dir.join("Data").join(name);
        if p.is_file() {
            return std::fs::read(p).ok();
        }
        self.archive.as_ref()?.read(&format!("data/{}", name))
    }
}

/// The game's scripts as (name, source), or None if they can't be read.
pub fn read_scripts(files: &GameFiles, ver: usize) -> Option<Vec<(String, Vec<u8>)>> {
    let raw = files.read_data(SCRIPT_FILES.get(ver)?)?;
    let Value::Array(entries) = marshal::load(&raw).ok()? else { return None };
    let mut out = Vec::new();
    for e in entries {
        let Value::Array(parts) = e else { continue };
        let (Some(name), Some(Value::Str(code))) = (parts.get(1), parts.get(2)) else { continue };
        let src = inflate(code)?;
        out.push((String::from_utf8_lossy(name.as_bytes().unwrap_or(b"")).into_owned(), src));
    }
    Some(out)
}

// Strings only the editor shows; they never reach the screen, so they don't need glyphs.
const EDITOR_ONLY_IVARS: [&str; 3] = ["@note", "@switches", "@variables"];
const EDITOR_ONLY_NAMES: [&str; 7] = ["RPG::Event", "RPG::CommonEvent", "RPG::MapInfo", "RPG::Tileset",
                                      "RPG::Troop", "RPG::Animation", "RPG::Map"];
const EDITOR_ONLY_COMMANDS: [i64; 4] = [108, 408, 118, 119]; // comments, labels

fn collect_strings(v: &Value, out: &mut HashSet<char>) {
    match v {
        Value::Str(s) => {
            // Real text is UTF-8; anything else is binary (e.g. zlib-packed code
            // some games hide in oddly named .rvdata files) and never shown.
            if let Ok(t) = std::str::from_utf8(s) {
                out.extend(t.chars());
            }
        }
        Value::Array(a) => a.iter().for_each(|x| collect_strings(x, out)),
        Value::Hash(h) => h.iter().for_each(|(_, x)| collect_strings(x, out)),
        Value::Object { class, ivars } => {
            if class == "RPG::EventCommand" {
                if let Some(code) = v.ivar("@code").and_then(|c| c.as_int()) {
                    if EDITOR_ONLY_COMMANDS.contains(&code) {
                        return;
                    }
                }
            }
            for (k, x) in ivars {
                if EDITOR_ONLY_IVARS.contains(&k.as_str())
                    || (k == "@name" && EDITOR_ONLY_NAMES.contains(&class.as_str())) {
                    continue;
                }
                collect_strings(x, out);
            }
        }
        _ => {}
    }
}

/// Characters the game can put on screen (dialogue, choices, database names and
/// terms). Big games have 1000+ data files, so they're spread over all cores.
pub fn collect_text_chars(files: &GameFiles, ver: usize) -> HashSet<char> {
    let names: Vec<String> = files.data_names(DATA_EXTS[ver]).into_iter()
        .filter(|n| {
            let base = n.rsplit('/').next().unwrap_or(n).to_lowercase();
            !base.starts_with("scripts") && !base.starts_with("mapinfos")
        })
        .collect();
    let threads = std::thread::available_parallelism().map_or(1, |n| n.get()).min(names.len().max(1));
    let chunks: Vec<Vec<&String>> = (0..threads)
        .map(|t| names.iter().skip(t).step_by(threads).collect())
        .collect();
    let mut chars = HashSet::new();
    std::thread::scope(|s| {
        let handles: Vec<_> = chunks.iter().map(|chunk| s.spawn(move || {
            let mut set = HashSet::new();
            for name in chunk {
                if let Some(raw) = files.read_data(name) {
                    if let Ok(v) = marshal::load(&raw) {
                        collect_strings(&v, &mut set);
                    }
                }
            }
            set
        })).collect();
        for h in handles {
            if let Ok(set) = h.join() {
                chars.extend(set);
            }
        }
    });
    chars.retain(|c| (*c as u32) >= 0x80 && !c.is_whitespace() && !c.is_control() && !is_invisible_format(*c));
    chars
}

/// Invisible format characters (Unicode category Cf: zero-width space/joiners,
/// direction marks, soft hyphen, BOM ...). Fonts needn't have glyphs for them.
fn is_invisible_format(c: char) -> bool {
    matches!(c as u32, 0xAD | 0x600..=0x605 | 0x61C | 0x6DD | 0x70F | 0x180E | 0x200B..=0x200F
        | 0x202A..=0x202E | 0x2060..=0x2064 | 0x2066..=0x206F | 0xFEFF | 0xFFF9..=0xFFFB
        | 0x110BD | 0x1D173..=0x1D17A | 0xE0001 | 0xE0020..=0xE007F)
}

pub fn has_midi(dir: &Path) -> bool {
    fn walk(d: &Path) -> bool {
        let Ok(rd) = std::fs::read_dir(d) else { return false };
        rd.flatten().any(|e| {
            let p = e.path();
            if p.is_dir() {
                walk(&p)
            } else {
                p.extension().map_or(false, |x| x.eq_ignore_ascii_case("mid") || x.eq_ignore_ascii_case("midi"))
            }
        })
    }
    walk(&dir.join("Audio"))
}

// ---------------------------------------------------------------- RTP

const RTP_DIRS: [&[&str]; 4] = [&[], &["RGSS/Standard"], &["RGSS2/RPGVX"], &["RPGVXAce/RTP", "RGSS3/RPGVXAce"]];

pub fn find_rtp(ver: usize, explicit: Option<&Path>) -> Result<Option<PathBuf>, String> {
    if let Some(p) = explicit {
        return if p.is_dir() { Ok(Some(p.to_path_buf())) } else { Err(format!("RTP folder not found: {}", p.display())) };
    }
    let home = crate::util::home_dir();
    let roots = [
        home.join(".wine/drive_c/Program Files (x86)/Common Files/Enterbrain"),
        home.join(".wine/drive_c/Program Files/Common Files/Enterbrain"),
        home.join("RTP"),
        PathBuf::from("C:\\Program Files (x86)\\Common Files\\Enterbrain"),
        PathBuf::from("C:\\Program Files\\Common Files\\Enterbrain"),
    ];
    for root in &roots {
        for sub in RTP_DIRS[ver] {
            let p = root.join(sub);
            if p.join("Graphics").is_dir() {
                return Ok(Some(p));
            }
        }
    }
    Ok(None)
}

/// Heuristic: the game declares an RTP and doesn't ship its own key system graphics.
pub fn needs_rtp(dir: &Path, ini: &HashMap<String, String>, ver: usize) -> bool {
    if !ini.iter().any(|(k, v)| k.to_uppercase().starts_with("RTP") && !v.trim().is_empty()) {
        return false;
    }
    if find_archive(dir, ver).is_some() {
        return false; // encrypted: can't inspect; most ship their assets inside the archive
    }
    let sys = dir.join("Graphics").join(if ver == 1 { "Windowskins" } else { "System" });
    let Ok(rd) = std::fs::read_dir(&sys) else { return true };
    let names: Vec<String> = rd.flatten()
        .map(|e| e.path().file_stem().map(|s| s.to_string_lossy().to_lowercase()).unwrap_or_default())
        .collect();
    !names.iter().any(|n| n == "window" || n.starts_with("001-"))
}
