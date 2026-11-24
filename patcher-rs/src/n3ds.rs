//! Nintendo 3DS: a game for MKXP-Z 3DS (ports/3ds), the patcher's port of
//! ports/3ds/tools (make-3ds-package.py and the scripts it runs), so no
//! Python, ImageMagick, lz4 or ffmpeg is needed.
//!
//! Made next to the game folder: "<game> (3DS)" with
//!   3ds/MKXP-Z 3DS.3dsx      the one app for all games (built by
//!                            ports/3ds/build.sh, carried in the patcher)
//!   mkxpz/<game>/Game.data   the game's files in one pack (images as .mkxi,
//!                            sound as .mkxa; an RGSS archive unpacked)
//!   mkxpz/<game>/game.smdh   title and icon for the app's game list
//!   mkxpz/<game>/Game.ini, Fonts/, the game folder's own files (plain)
//! Copied to the SD card's top, the app (in sdmc:/3ds/) lists the game.
//!
//! Formats (the engine reads them; ports/3ds/tools has the reference):
//! .mkxi  "MKXI", u32 version | format << 8 (0x80: decided), w, h; version
//!        1: the RGBA pixels as one LZ4 frame (64 KB blocks); version 2
//!        (4 MB of pixels and more): 128x128 tiles, u32 tile size, u32
//!        count, per tile u32 offset, u32 length (0: transparent), each tile
//!        an LZ4 frame (identical tiles stored once)
//! .mkxa  DSP-ADPCM, see encode_mkxa
//! Game.data  "MKXPZPK1", u32 count, u32 index length; per file (sorted by
//!        the lowercase name) u32 offset, u32 size, u16 name length, name;
//!        then the files

use crate::archive::Archive;
use crate::game;
use std::collections::BTreeMap;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Mutex;

/// ports/3ds/build.sh's app, zlib-compressed (made by that script)
static APP_3DSX: &[u8] = include_bytes!("../assets/3ds/mkxpz-3ds.3dsx.z");
/// the app's own icon: the list's icon for games without one
static GENERIC_ICON: &[u8] = include_bytes!("../../ports/3ds/icon.png");

pub const APP_NAME: &str = "MKXP-Z 3DS.3dsx";

#[derive(Clone, Default)]
pub struct Options {
    /// the RTP folder, when the game needs one (its files go into the pack)
    pub rtp: Option<PathBuf>,
    /// leave the sound as it is (the engine plays Ogg, WAV, MP3 itself,
    /// slower than .mkxa)
    pub keep_audio: bool,
    /// a SoundFont for rendering MIDI music; None: an installed one
    pub soundfont: Option<PathBuf>,
    /// where "<game> (3DS)" goes (default: next to the game folder)
    pub out_parent: Option<PathBuf>,
}

const SKIP_EXT: &[&str] = &["exe", "dll", "rgssad", "rgss2a", "rgss3a", "rxproj", "rvproj", "rvproj2", "lnk", "url",
                            "ppdb", "zip", "7z", "rar", "psd", "pdf", "html", "htm", "ifl"];
const SKIP_NAMES: &[&str] = &["thumbs.db", "desktop.ini"];
const PLAIN_EXT: &[&str] = &["txt", "ini", "cfg", "json"];
const IMAGE_EXT: &[&str] = &["png", "jpg", "jpeg", "bmp"];
const AUDIO_EXT: &[&str] = &["ogg", "oga", "wav", "mp3", "mid", "midi", "flac"];
const DSP_RATE: u32 = 32728;

fn ext_of(name: &str) -> String {
    name.rsplit_once('.').map(|(_, e)| e.to_ascii_lowercase()).unwrap_or_default()
}

fn is_save(name: &str) -> bool {
    let low = name.to_ascii_lowercase();
    low.starts_with("save") && (low.ends_with(".rxdata") || low.ends_with(".rvdata") || low.ends_with(".rvdata2"))
}

/// a file of the game: from the folder, the archive, or made here
#[derive(Clone)]
enum Src {
    File(PathBuf),
    Archive(usize, String),
    Data(std::sync::Arc<Vec<u8>>),
}

struct Item {
    /// as the game spells it, forward slashes
    shown: String,
    src: Src,
}

fn read_src(src: &Src, archives: &[Archive]) -> Result<Vec<u8>, String> {
    match src {
        Src::File(p) => std::fs::read(p).map_err(|e| format!("{}: {}", p.display(), e)),
        Src::Archive(i, n) => archives[*i].read(n).ok_or_else(|| format!("{}: not in the archive", n)),
        Src::Data(d) => Ok(d.as_ref().clone()),
    }
}

/// a name for the SD card (FAT): no reserved characters
fn fat_name(s: &str) -> String {
    let n: String = s.chars().filter(|c| !matches!(c, '\\' | '/' | ':' | '*' | '?' | '"' | '<' | '>' | '|') && !c.is_control())
        .collect();
    let n = n.trim_matches(|c| c == ' ' || c == '.').to_string();
    if n.is_empty() { "game".into() } else { n }
}

/// Make the 3DS folder for `game`; returns its path.
pub fn create(game_dir: &Path, o: &Options, log: &dyn Fn(String)) -> Result<PathBuf, String> {
    let folder = game_dir.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_else(|| "game".into());
    let parent = o.out_parent.clone().or_else(|| game_dir.parent().map(Path::to_path_buf)).unwrap_or_default();
    let out = parent.join(format!("{} (3DS)", folder));
    let sd_name = fat_name(&folder);
    let game_out = out.join("mkxpz").join(&sd_name);
    if out.exists() {
        // (made by an earlier run: replaced; anything else is left alone)
        if !out.join("mkxpz").is_dir() {
            return Err(format!("{} exists and isn't a 3DS folder made by the patcher", out.display()));
        }
        crate::util::remove_path(&out).map_err(|e| format!("{}: {}", out.display(), e))?;
    }
    let (ini_path, ini) = game::read_game_ini(game_dir);
    let ver = game::detect_rgss(game_dir, &ini);

    // 1. the game's files (the folder, its archive, the RTP when needed)
    log("3DS: collecting the game's files ...".into());
    let mut files: BTreeMap<String, Item> = BTreeMap::new();
    let mut archives = Vec::new();
    fn walk(dir: &Path, rel: &str, files: &mut BTreeMap<String, Item>, top: bool) {
        let Ok(rd) = std::fs::read_dir(dir) else { return };
        let mut ents: Vec<_> = rd.flatten().collect();
        ents.sort_by_key(|e| e.file_name());
        for e in ents {
            let name = e.file_name().to_string_lossy().into_owned();
            if name.starts_with('.') {
                continue;
            }
            let path = e.path();
            let shown = if rel.is_empty() { name.clone() } else { format!("{}/{}", rel, name) };
            if path.is_dir() {
                if !(top && name.eq_ignore_ascii_case("mkxp-z")) {
                    walk(&path, &shown, files, false);
                }
            } else if !SKIP_EXT.contains(&ext_of(&name).as_str()) && !(top && is_save(&name)) {
                files.insert(shown.to_lowercase(), Item { shown, src: Src::File(path) });
            }
        }
    }
    walk(game_dir, "", &mut files, true);
    if let Ok(rd) = std::fs::read_dir(game_dir) {
        for e in rd.flatten() {
            let name = e.file_name().to_string_lossy().into_owned();
            if matches!(ext_of(&name).as_str(), "rgssad" | "rgss2a" | "rgss3a") {
                let a = Archive::open(&e.path())?;
                let i = archives.len();
                for n in a.original_names() {
                    files.insert(n.to_lowercase(), Item { shown: n.clone(), src: Src::Archive(i, n.clone()) });
                }
                log(format!("3DS: unpacking {} ({} files)", name, a.original_names().len()));
                archives.push(a);
            }
        }
    }
    // (the game's .ini under another name, e.g. "Pocket Mirror.ini": the
    // engine reads Game.ini)
    if let Some(p) = &ini_path {
        let low = p.file_name().map(|n| n.to_string_lossy().to_lowercase()).unwrap_or_default();
        if low != "game.ini" {
            if let Some(it) = files.remove(&low) {
                files.insert("game.ini".into(), Item { shown: "Game.ini".into(), src: it.src });
            }
        }
    }
    if game::needs_rtp(game_dir, &ini, ver) {
        match game::find_rtp(ver, o.rtp.as_deref())? {
            Some(rtp) => {
                let mut extra = BTreeMap::new();
                for sub in ["Graphics", "Audio"] {
                    walk(&rtp.join(sub), sub, &mut extra, false);
                }
                let n = extra.len();
                for (k, v) in extra {
                    files.entry(k).or_insert(v);
                }
                log(format!("3DS: the RTP's graphics and sound added ({} files, the game's own come first)", n));
            }
            None => log("3DS: warning: the game seems to need the RTP, which wasn't found (choose its folder); \
                         graphics or sounds may be missing".into()),
        }
    }

    // 2. images -> .mkxi, sound -> .mkxa
    let image_keys: Vec<String> = files.iter()
        .filter(|(k, _)| k.starts_with("graphics/") && IMAGE_EXT.contains(&ext_of(k).as_str()))
        .map(|(k, _)| k.clone()).collect();
    log(format!("3DS: converting {} images ...", image_keys.len()));
    let conv = convert_all(&image_keys, &files, &archives, |data| {
        let (w, h, px) = decode_image(&data)?;
        Ok(mkxi(w, h, &px))
    });
    let (mut before, mut after, mut failed) = (0u64, 0u64, Vec::new());
    for (k, r) in image_keys.iter().zip(conv) {
        match r {
            Ok((orig, out)) => {
                before += orig as u64;
                after += out.len() as u64;
                replace_ext(&mut files, k, "mkxi", out);
            }
            Err(e) => failed.push(format!("{}: {}", files[k].shown, e)),
        }
    }
    log(format!("3DS: images: {} KB -> {} KB", before / 1024, after / 1024));
    if !o.keep_audio {
        let soundfont = o.soundfont.clone().or_else(|| crate::plan::find_soundfonts().into_iter().next());
        let synth_font = soundfont.as_ref().and_then(|p| {
            let mut f = std::fs::File::open(p).ok()?;
            rustysynth::SoundFont::new(&mut f).ok().map(std::sync::Arc::new)
        });
        let audio_keys: Vec<String> = files.iter()
            .filter(|(k, _)| k.starts_with("audio/") && AUDIO_EXT.contains(&ext_of(k).as_str()))
            .map(|(k, _)| k.clone()).collect();
        let midi_kept = audio_keys.iter().any(|k| matches!(ext_of(k).as_str(), "mid" | "midi")) && synth_font.is_none();
        log(format!("3DS: converting {} sounds to DSP-ADPCM ...{}", audio_keys.len(),
                    if midi_kept { " (MIDI kept: no SoundFont found)" } else { "" }));
        let conv = convert_all(&audio_keys, &files, &archives, |data| {
            let k = AUDIO_KEY.with(|c| c.borrow().clone());
            let music = ["audio/bgm/", "audio/bgs/", "audio/me/"].iter().any(|d| k.starts_with(d));
            let ext = ext_of(&k);
            if ext == "mid" || ext == "midi" {
                let sf = synth_font.as_ref().ok_or("kept (no SoundFont)")?;
                let (pcm, loop_start) = render_midi(sf, &data, DSP_RATE)?;
                Ok(encode_mkxa(&pcm, DSP_RATE, 2, loop_start, 0))
            } else {
                let a = decode_audio(&data, &ext)?;
                let rate = if music && a.rate > DSP_RATE { DSP_RATE } else { a.rate };
                let pcm = if rate != a.rate { resample(&a.pcm, a.channels, a.rate, rate) } else { a.pcm };
                let scale = |v: u64| (v * rate as u64 / a.rate as u64) as usize;
                let (ls, le) = match (a.loop_start, a.loop_length) {
                    (Some(s), Some(l)) if l > 0 => (scale(s), scale(s + l)),
                    _ => (0, 0),
                };
                Ok(encode_mkxa(&pcm, rate, a.channels, ls, le))
            }
        });
        let (mut before, mut after) = (0u64, 0u64);
        for (k, r) in audio_keys.iter().zip(conv) {
            match r {
                Ok((orig, out)) => {
                    before += orig as u64;
                    after += out.len() as u64;
                    replace_ext(&mut files, k, "mkxa", out);
                }
                Err(e) if e.starts_with("kept") => {}
                Err(e) => failed.push(format!("{}: {}", files[k].shown, e)),
            }
        }
        log(format!("3DS: sound: {} KB -> {} KB", before / 1024, after / 1024));
    }
    for f in failed.iter().take(20) {
        log(format!("3DS: kept as it is: {}", f));
    }

    // 3. plain files, the pack
    std::fs::create_dir_all(&game_out).map_err(|e| format!("{}: {}", game_out.display(), e))?;
    let mut packed: Vec<(&String, &Item)> = Vec::new();
    for (low, it) in &files {
        let base = low.rsplit('/').next().unwrap_or(low);
        let ext = ext_of(low);
        if SKIP_NAMES.contains(&base) || base.starts_with("uninstall") {
            continue;
        }
        // (the folder's own files stay plain: games write some, e.g. with "r+")
        if low.starts_with("fonts/") || PLAIN_EXT.contains(&ext.as_str()) || !low.contains('/') {
            let dst = game_out.join(&it.shown);
            if let Some(d) = dst.parent() {
                std::fs::create_dir_all(d).map_err(|e| format!("{}: {}", d.display(), e))?;
            }
            std::fs::write(&dst, read_src(&it.src, &archives)?).map_err(|e| format!("{}: {}", dst.display(), e))?;
            continue;
        }
        packed.push((low, it));
    }
    write_pack(&game_out.join("Game.data"), &packed, &archives)?;
    log(format!("3DS: Game.data with {} files", packed.len()));

    // 4. game.smdh (title and icon), the app
    let title = ini.get("Title").cloned().filter(|t| !t.trim().is_empty()).unwrap_or_else(|| folder.clone());
    let icon = game_icon(game_dir).unwrap_or_else(|| decode_png(GENERIC_ICON).unwrap_or((1, 1, vec![0; 4])));
    std::fs::write(game_out.join("game.smdh"), smdh(&title, "RPG Maker game (MKXP-Z 3DS)", "MKXP-Z Patcher", &icon))
        .map_err(|e| format!("game.smdh: {}", e))?;
    let app_dir = out.join("3ds");
    std::fs::create_dir_all(&app_dir).map_err(|e| format!("{}: {}", app_dir.display(), e))?;
    let app = miniz_oxide::inflate::decompress_to_vec_zlib(APP_3DSX).map_err(|_| "the patcher's 3DS app is damaged")?;
    std::fs::write(app_dir.join(APP_NAME), app).map_err(|e| format!("{}: {}", APP_NAME, e))?;
    std::fs::write(out.join("README.txt"), readme(&sd_name, &title)).map_err(|e| format!("README.txt: {}", e))?;
    log(format!("3DS: done: {}", out.display()));
    Ok(out)
}

fn readme(sd_name: &str, title: &str) -> String {
    format!("{title} for the Nintendo 3DS (MKXP-Z 3DS)\r\n\r\n\
             Needs a 3DS / 2DS / New 3DS with custom firmware (Luma3DS) and the Homebrew Launcher.\r\n\
             Copy both folders here to the top of the SD card:\r\n\
             \x20 3ds\\{app}    -> sdmc:/3ds/{app} (the one app for all games)\r\n\
             \x20 mkxpz\\{sd_name}  -> sdmc:/mkxpz/{sd_name}\r\n\r\n\
             Start \"MKXP-Z 3DS\" in the Homebrew Launcher: it lists the games in sdmc:/mkxpz/ (one game starts at once).\r\n\
             START in a game: settings (text size, screen, 3D...), back to the list, reset.\r\n\
             For sound the 3DS's DSP firmware must be on the SD card (sdmc:/3ds/dspfirm.cdc: run \"DSP1\" once).\r\n\
             Saves go into the game's folder on the SD card.\r\n", app = APP_NAME)
}

thread_local! {
    /// (the file being converted, for the converter's closure)
    static AUDIO_KEY: std::cell::RefCell<String> = std::cell::RefCell::new(String::new());
}

/// convert the files in parallel; per file (original size, result) or why not
fn convert_all<F>(keys: &[String], files: &BTreeMap<String, Item>, archives: &[Archive], f: F)
    -> Vec<Result<(usize, Vec<u8>), String>>
where F: Fn(Vec<u8>) -> Result<Vec<u8>, String> + Sync {
    let next = AtomicUsize::new(0);
    let results: Mutex<Vec<Option<Result<(usize, Vec<u8>), String>>>> = Mutex::new(vec![None; keys.len()]);
    let threads = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(4).min(16);
    std::thread::scope(|s| {
        for _ in 0..threads {
            s.spawn(|| loop {
                let i = next.fetch_add(1, Ordering::SeqCst);
                if i >= keys.len() {
                    break;
                }
                AUDIO_KEY.with(|c| *c.borrow_mut() = keys[i].clone());
                let r = read_src(&files[&keys[i]].src, archives).and_then(|d| {
                    let n = d.len();
                    f(d).map(|out| (n, out))
                });
                results.lock().unwrap()[i] = Some(r);
            });
        }
    });
    results.into_inner().unwrap().into_iter().map(|r| r.unwrap_or_else(|| Err("not converted".into()))).collect()
}

/// the converted file instead of the original (same path, another extension)
fn replace_ext(files: &mut BTreeMap<String, Item>, key: &str, ext: &str, data: Vec<u8>) {
    let Some(it) = files.remove(key) else { return };
    let stem = |s: &str| s.rsplit_once('.').map(|(a, _)| a.to_string()).unwrap_or_else(|| s.to_string());
    let shown = format!("{}.{}", stem(&it.shown), ext);
    files.insert(format!("{}.{}", stem(key), ext), Item { shown, src: Src::Data(std::sync::Arc::new(data)) });
}

fn write_pack(path: &Path, items: &[(&String, &Item)], archives: &[Archive]) -> Result<(), String> {
    // (sorted by the lowercase name, which the engine looks up; the names
    // as the game spells them, for folder listings)
    let err = |e: std::io::Error| format!("{}: {}", path.display(), e);
    let mut sizes = Vec::with_capacity(items.len());
    for (_, it) in items {
        sizes.push(match &it.src {
            Src::File(p) => std::fs::metadata(p).map(|m| m.len() as usize).map_err(err)?,
            Src::Data(d) => d.len(),
            Src::Archive(i, n) => archives[*i].read(n).map(|d| d.len()).unwrap_or(0),
        });
    }
    let names: Vec<&[u8]> = items.iter().map(|(_, it)| it.shown.as_bytes()).collect();
    let ilen: usize = names.iter().map(|n| 10 + n.len()).sum();
    let mut index = Vec::with_capacity(ilen);
    let mut at = 16 + ilen;
    for (n, size) in names.iter().zip(&sizes) {
        index.extend_from_slice(&(at as u32).to_le_bytes());
        index.extend_from_slice(&(*size as u32).to_le_bytes());
        index.extend_from_slice(&(n.len() as u16).to_le_bytes());
        index.extend_from_slice(n);
        at += size;
    }
    if at > u32::MAX as usize {
        return Err("the game is too big for a 3DS pack (over 4 GB)".into());
    }
    let mut f = std::io::BufWriter::new(std::fs::File::create(path).map_err(err)?);
    f.write_all(b"MKXPZPK1").map_err(err)?;
    f.write_all(&(items.len() as u32).to_le_bytes()).map_err(err)?;
    f.write_all(&(ilen as u32).to_le_bytes()).map_err(err)?;
    f.write_all(&index).map_err(err)?;
    for ((_, it), size) in items.iter().zip(&sizes) {
        let d = read_src(&it.src, archives)?;
        if d.len() != *size {
            return Err(format!("{}: changed while packing", it.shown));
        }
        f.write_all(&d).map_err(err)?;
    }
    f.flush().map_err(err)
}

// ------------------------------------------------------------ images

pub fn decode_png(data: &[u8]) -> Result<(u32, u32, Vec<u8>), String> {
    let mut dec = png::Decoder::new(data);
    dec.set_transformations(png::Transformations::normalize_to_color8());
    let mut r = dec.read_info().map_err(|e| e.to_string())?;
    let mut buf = vec![0; r.output_buffer_size()];
    let info = r.next_frame(&mut buf).map_err(|e| e.to_string())?;
    let (w, h) = (info.width, info.height);
    let n = (w * h) as usize;
    let mut px = Vec::with_capacity(n * 4);
    match info.color_type {
        png::ColorType::Rgba => px.extend_from_slice(&buf[..n * 4]),
        png::ColorType::Rgb => buf[..n * 3].chunks(3).for_each(|c| px.extend_from_slice(&[c[0], c[1], c[2], 255])),
        png::ColorType::Grayscale => buf[..n].iter().for_each(|&g| px.extend_from_slice(&[g, g, g, 255])),
        png::ColorType::GrayscaleAlpha => buf[..n * 2].chunks(2).for_each(|c| px.extend_from_slice(&[c[0], c[0], c[0], c[1]])),
        png::ColorType::Indexed => return Err("palette not expanded".into()),
    }
    Ok((w, h, px))
}

fn decode_jpeg(data: &[u8]) -> Result<(u32, u32, Vec<u8>), String> {
    let mut dec = jpeg_decoder::Decoder::new(data);
    let pix = dec.decode().map_err(|e| e.to_string())?;
    let info = dec.info().ok_or("no JPEG info")?;
    let n = info.width as usize * info.height as usize;
    let mut px = Vec::with_capacity(n * 4);
    match info.pixel_format {
        jpeg_decoder::PixelFormat::RGB24 => pix.chunks(3).take(n).for_each(|c| px.extend_from_slice(&[c[0], c[1], c[2], 255])),
        jpeg_decoder::PixelFormat::L8 => pix.iter().take(n).for_each(|&g| px.extend_from_slice(&[g, g, g, 255])),
        jpeg_decoder::PixelFormat::L16 => pix.chunks(2).take(n).for_each(|c| px.extend_from_slice(&[c[0], c[0], c[0], 255])),
        jpeg_decoder::PixelFormat::CMYK32 => pix.chunks(4).take(n).for_each(|c| {
            let k = 255 - c[3] as u32;
            let f = |v: u8| ((255 - v as u32) * k / 255) as u8;
            px.extend_from_slice(&[f(c[0]), f(c[1]), f(c[2]), 255]);
        }),
    }
    Ok((info.width as u32, info.height as u32, px))
}

/// Windows BMP: 1/4/8-bit with a palette, 16 (5-5-5), 24, 32 bits; not compressed
fn decode_bmp(d: &[u8]) -> Result<(u32, u32, Vec<u8>), String> {
    let u16at = |o: usize| d.get(o..o + 2).map(|b| u16::from_le_bytes([b[0], b[1]])).ok_or("short BMP");
    let u32at = |o: usize| d.get(o..o + 4).map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]])).ok_or("short BMP");
    if d.get(..2) != Some(b"BM") {
        return Err("not a BMP".into());
    }
    let off = u32at(10)? as usize;
    let hsize = u32at(14)? as usize;
    let w = u32at(18)? as i32;
    let hraw = u32at(22)? as i32;
    let bpp = u16at(28)? as usize;
    let comp = u32at(30)?;
    if w <= 0 || hraw == 0 || !(comp == 0 || (comp == 3 && bpp == 32)) {
        return Err("unsupported BMP".into());
    }
    let (w, h, flip) = (w as usize, hraw.unsigned_abs() as usize, hraw > 0);
    let ncolors = if bpp <= 8 { let c = u32at(46)? as usize; if c == 0 { 1 << bpp } else { c } } else { 0 };
    let pal = d.get(14 + hsize..14 + hsize + ncolors * 4).ok_or("short BMP palette")?;
    let stride = (w * bpp + 31) / 32 * 4;
    let mut px = vec![0u8; w * h * 4];
    for y in 0..h {
        let sy = if flip { h - 1 - y } else { y };
        let row = d.get(off + sy * stride..off + sy * stride + stride).ok_or("short BMP data")?;
        for x in 0..w {
            let (r, g, b, a) = match bpp {
                32 => (row[x * 4 + 2], row[x * 4 + 1], row[x * 4], 255),
                24 => (row[x * 3 + 2], row[x * 3 + 1], row[x * 3], 255),
                16 => {
                    let v = u16::from_le_bytes([row[x * 2], row[x * 2 + 1]]);
                    let c = |s: u32| (((v >> s) & 31) as u32 * 255 / 31) as u8;
                    (c(10), c(5), c(0), 255)
                }
                1 | 4 | 8 => {
                    let bit = x * bpp;
                    let i = ((row[bit / 8] >> (8 - bpp - bit % 8)) as usize) & ((1 << bpp) - 1);
                    let c = pal.get(i * 4..i * 4 + 3).ok_or("BMP palette")?;
                    (c[2], c[1], c[0], 255)
                }
                _ => return Err("unsupported BMP depth".into()),
            };
            px[(y * w + x) * 4..(y * w + x) * 4 + 4].copy_from_slice(&[r, g, b, a]);
        }
    }
    Ok((w as u32, h as u32, px))
}

fn decode_image(data: &[u8]) -> Result<(u32, u32, Vec<u8>), String> {
    if data.starts_with(b"\x89PNG") {
        decode_png(data)
    } else if data.starts_with(&[0xff, 0xd8]) {
        decode_jpeg(data)
    } else if data.starts_with(b"BM") {
        decode_bmp(data)
    } else {
        Err("not a PNG, JPEG or BMP".into())
    }
}

const TILE: u32 = 128;
const TILED_MIN: usize = 4 * 1024 * 1024;
const FMT_DECIDED: u32 = 0x80;

/// the smallest GPU texture format holding every pixel exactly (also the
/// colour of invisible ones: the GPU's filtering mixes it in): 0 RGBA8,
/// 1 RGB8, 2 LA8, 3 L8, 4 LA4, 5 L4, 6 A8, 7 A4
fn exact_format(px: &[u8]) -> u32 {
    let (mut opaque, mut grey, mut black, mut a4, mut l4) = (true, true, true, true, true);
    for p in px.chunks(4) {
        let (r, g, b, a) = (p[0], p[1], p[2], p[3]);
        opaque &= a == 255;
        grey &= r == g && g == b;
        black &= r == 0 && r == g && g == b;
        a4 &= a % 17 == 0;
        l4 &= r % 17 == 0;
    }
    if black && !opaque {
        return if a4 { 7 } else { 6 };
    }
    if grey && opaque {
        return if l4 { 5 } else { 3 };
    }
    if grey {
        return if l4 && a4 { 4 } else { 2 };
    }
    if opaque { 1 } else { 0 }
}

/// an LZ4 frame with 64 KB blocks (the engine unpacks it block by block)
fn lz4_frame(data: &[u8]) -> Vec<u8> {
    let info = lz4_flex::frame::FrameInfo::new().block_size(lz4_flex::frame::BlockSize::Max64KB);
    let mut enc = lz4_flex::frame::FrameEncoder::with_frame_info(info, Vec::with_capacity(data.len() / 2));
    enc.write_all(data).expect("lz4 in memory");
    enc.finish().expect("lz4 in memory")
}

fn mkxi(w: u32, h: u32, px: &[u8]) -> Vec<u8> {
    let fmt = exact_format(px);
    let mut out = b"MKXI".to_vec();
    if (w * h * 4) as usize >= TILED_MIN {
        let (tx, ty) = ((w + TILE - 1) / TILE, (h + TILE - 1) / TILE);
        for v in [2 | (fmt | FMT_DECIDED) << 8, w, h, TILE, tx * ty] {
            out.extend_from_slice(&v.to_le_bytes());
        }
        let mut at = out.len() + 8 * (tx * ty) as usize;
        let (mut table, mut data) = (Vec::new(), Vec::new());
        let mut seen: std::collections::HashMap<(u32, u32, Vec<u8>), (u32, u32)> = Default::default();
        for j in 0..ty {
            for i in 0..tx {
                let (x0, y0) = (i * TILE, j * TILE);
                let (tw, th) = (TILE.min(w - x0), TILE.min(h - y0));
                let mut rows = Vec::with_capacity((tw * th * 4) as usize);
                for y in 0..th {
                    let o = (((y0 + y) * w + x0) * 4) as usize;
                    rows.extend_from_slice(&px[o..o + (tw * 4) as usize]);
                }
                if rows.chunks(4).all(|p| p[3] == 0) {
                    table.push((0u32, 0u32));
                    continue;
                }
                let key = (tw, th, rows);
                if let Some(e) = seen.get(&key) {
                    table.push(*e);
                    continue;
                }
                let packed = lz4_frame(&key.2);
                let e = (at as u32, packed.len() as u32);
                table.push(e);
                seen.insert(key, e);
                at += packed.len();
                data.extend_from_slice(&packed);
            }
        }
        for (o, n) in table {
            out.extend_from_slice(&o.to_le_bytes());
            out.extend_from_slice(&n.to_le_bytes());
        }
        out.extend_from_slice(&data);
    } else {
        for v in [1 | (fmt | FMT_DECIDED) << 8, w, h] {
            out.extend_from_slice(&v.to_le_bytes());
        }
        out.extend_from_slice(&lz4_frame(px));
    }
    out
}

// ------------------------------------------------------------ icon, SMDH

/// the game's icon (Game.exe's, or the exe named like its .ini) as RGBA
fn game_icon(dir: &Path) -> Option<(u32, u32, Vec<u8>)> {
    let mut exes: Vec<PathBuf> = std::fs::read_dir(dir).ok()?.flatten().map(|e| e.path())
        .filter(|p| p.extension().map_or(false, |e| e.eq_ignore_ascii_case("exe"))).collect();
    exes.sort_by_key(|p| (!p.file_name().map_or(false, |n| n.eq_ignore_ascii_case("game.exe")), p.clone()));
    for exe in exes {
        let low = exe.file_name()?.to_string_lossy().to_lowercase();
        if low.contains("unins") || low.contains("setup") || low.contains("mkxp") {
            continue;
        }
        let d = std::fs::read(&exe).ok()?;
        if let Some(i) = crate::pe::icon_from_exe(&d).and_then(|ic| crate::pe::largest_rgba(&ic)) {
            return Some(i);
        }
    }
    None
}

/// RGBA scaled into an n x n square (aspect kept, area average), on black
fn icon_square(img: &(u32, u32, Vec<u8>), n: u32) -> Vec<[u8; 3]> {
    let (w, h, px) = (img.0.max(1), img.1.max(1), &img.2);
    let s = n as f32 / w.max(h) as f32;
    let (dw, dh) = (((w as f32 * s).round() as u32).max(1), ((h as f32 * s).round() as u32).max(1));
    let (ox, oy) = ((n - dw.min(n)) / 2, (n - dh.min(n)) / 2);
    let mut out = vec![[0u8; 3]; (n * n) as usize];
    for y in 0..dh.min(n) {
        for x in 0..dw.min(n) {
            // (the source pixels this one covers)
            let (sx0, sx1) = ((x * w / dw), (((x + 1) * w + dw - 1) / dw).max(x * w / dw + 1).min(w));
            let (sy0, sy1) = ((y * h / dh), (((y + 1) * h + dh - 1) / dh).max(y * h / dh + 1).min(h));
            let mut acc = [0u32; 3];
            let mut cnt = 0u32;
            for sy in sy0..sy1 {
                for sx in sx0..sx1 {
                    let p = &px[((sy * w + sx) * 4) as usize..((sy * w + sx) * 4 + 4) as usize];
                    let a = p[3] as u32;
                    for c in 0..3 {
                        acc[c] += p[c] as u32 * a / 255;
                    }
                    cnt += 1;
                }
            }
            let v = |c: usize| (acc[c] / cnt.max(1)) as u8;
            out[((oy + y) * n + ox + x) as usize] = [v(0), v(1), v(2)];
        }
    }
    out
}

/// an icon as the 3DS stores it: RGB565 in 8x8 tiles, Morton order inside
fn tiled565(px: &[[u8; 3]], n: u32) -> Vec<u8> {
    let mut out = vec![0u8; (n * n * 2) as usize];
    for y in 0..n {
        for x in 0..n {
            let m = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
            let i = (((y >> 3) * (n / 8) + (x >> 3)) * 64 + m) as usize;
            let [r, g, b] = px[(y * n + x) as usize];
            let v = (r as u16 >> 3) << 11 | (g as u16 >> 2) << 5 | b as u16 >> 3;
            out[i * 2..i * 2 + 2].copy_from_slice(&v.to_le_bytes());
        }
    }
    out
}

/// an SMDH (the app's metadata: titles in 16 languages, two icons)
fn smdh(title: &str, desc: &str, publisher: &str, icon: &(u32, u32, Vec<u8>)) -> Vec<u8> {
    let mut d = vec![0u8; 0x36C0];
    d[..4].copy_from_slice(b"SMDH");
    let put16 = |d: &mut Vec<u8>, at: usize, s: &str, max: usize| {
        for (i, u) in s.encode_utf16().take(max - 1).enumerate() {
            d[at + i * 2..at + i * 2 + 2].copy_from_slice(&u.to_le_bytes());
        }
    };
    for lang in 0..16 {
        let at = 8 + lang * 0x200;
        put16(&mut d, at, title, 0x40);
        put16(&mut d, at + 0x80, desc, 0x80);
        put16(&mut d, at + 0x180, publisher, 0x40);
    }
    d[0x2018..0x201C].copy_from_slice(&0x7FFF_FFFFu32.to_le_bytes()); // region free
    d[0x2028..0x202C].copy_from_slice(&0x0005u32.to_le_bytes()); // visible, 3D allowed
    d[0x2040..0x24C0].copy_from_slice(&tiled565(&icon_square(icon, 24), 24));
    d[0x24C0..0x36C0].copy_from_slice(&tiled565(&icon_square(icon, 48), 48));
    d
}

// ------------------------------------------------------------ sound

struct Pcm {
    pcm: Vec<i16>,
    rate: u32,
    channels: usize,
    loop_start: Option<u64>,
    loop_length: Option<u64>,
}

fn decode_audio(data: &[u8], ext: &str) -> Result<Pcm, String> {
    use symphonia::core::{audio::SampleBuffer, codecs::DecoderOptions, formats::FormatOptions,
                          io::MediaSourceStream, meta::MetadataOptions, probe::Hint};
    let mss = MediaSourceStream::new(Box::new(std::io::Cursor::new(data.to_vec())), Default::default());
    let mut hint = Hint::new();
    hint.with_extension(ext);
    let mut probed = symphonia::default::get_probe()
        .format(&hint, mss, &FormatOptions::default(), &MetadataOptions::default())
        .map_err(|e| format!("not a sound file ({})", e))?;
    let mut loop_start = None;
    let mut loop_length = None;
    let mut tags = |rev: &symphonia::core::meta::MetadataRevision| {
        for t in rev.tags() {
            let v = t.value.to_string().trim().parse::<u64>().ok();
            match t.key.to_ascii_uppercase().as_str() {
                "LOOPSTART" => loop_start = loop_start.or(v),
                "LOOPLENGTH" => loop_length = loop_length.or(v),
                _ => {}
            }
        }
    };
    if let Some(m) = probed.metadata.get() {
        if let Some(r) = m.current() {
            tags(r);
        }
    }
    let mut format = probed.format;
    if let Some(r) = format.metadata().current() {
        tags(r);
    }
    let track = format.default_track().ok_or("no sound in it")?.clone();
    let mut dec = symphonia::default::get_codecs().make(&track.codec_params, &DecoderOptions::default())
        .map_err(|e| e.to_string())?;
    let rate = track.codec_params.sample_rate.ok_or("no sample rate")?;
    let mut channels = 0usize;
    let mut pcm = Vec::new();
    let mut buf: Option<SampleBuffer<i16>> = None;
    loop {
        let pkt = match format.next_packet() {
            Ok(p) => p,
            Err(symphonia::core::errors::Error::IoError(_)) => break,
            Err(symphonia::core::errors::Error::ResetRequired) => break,
            Err(e) => return Err(e.to_string()),
        };
        if pkt.track_id() != track.id {
            continue;
        }
        let decoded = match dec.decode(&pkt) {
            Ok(d) => d,
            Err(symphonia::core::errors::Error::DecodeError(_)) => continue,
            Err(e) => return Err(e.to_string()),
        };
        let spec = *decoded.spec();
        let ch = spec.channels.count();
        if buf.as_ref().map_or(true, |b| b.capacity() < decoded.capacity() * ch) {
            buf = Some(SampleBuffer::new(decoded.capacity() as u64, spec));
        }
        let b = buf.as_mut().unwrap();
        b.copy_interleaved_ref(decoded);
        channels = ch;
        // (more than 2 channels: the first two)
        let keep = ch.min(2);
        for frame in b.samples().chunks(ch) {
            pcm.extend_from_slice(&frame[..keep]);
        }
    }
    if channels == 0 {
        return Err("no sound decoded".into());
    }
    Ok(Pcm { pcm, rate, channels: channels.min(2), loop_start, loop_length })
}

/// rate conversion: windowed sinc (Blackman, 32 taps), low-passed below
/// the lower rate; the weights for 512 positions between two samples
/// computed once
fn resample(pcm: &[i16], ch: usize, from: u32, to: u32) -> Vec<i16> {
    const HALF: usize = 16;
    const PHASES: usize = 512;
    let frames = pcm.len() / ch;
    let out_frames = (frames as u64 * to as u64 / from as u64) as usize;
    let ratio = from as f64 / to as f64;
    let cut = (to as f64 / from as f64).min(1.0) * 0.95;
    let pi = std::f64::consts::PI;
    let mut table = vec![0f32; PHASES * 2 * HALF];
    for ph in 0..PHASES {
        let frac = ph as f64 / PHASES as f64;
        let mut row = [0f64; 2 * HALF];
        let mut sum = 0.0;
        for (j, r) in row.iter_mut().enumerate() {
            // (tap j: the sample at floor(t) - HALF + 1 + j)
            let x = frac + HALF as f64 - 1.0 - j as f64;
            let s = if x.abs() < 1e-9 { 1.0 } else { (pi * x * cut).sin() / (pi * x * cut) };
            let wpos = (x / HALF as f64 + 1.0) / 2.0;
            let w = if (0.0..=1.0).contains(&wpos) {
                0.42 - 0.5 * (2.0 * pi * wpos).cos() + 0.08 * (4.0 * pi * wpos).cos()
            } else { 0.0 };
            *r = s * w;
            sum += *r;
        }
        for j in 0..2 * HALF {
            table[ph * 2 * HALF + j] = (row[j] / sum) as f32;
        }
    }
    let mut out = Vec::with_capacity(out_frames * ch);
    for i in 0..out_frames {
        let t = i as f64 * ratio;
        let c = t.floor() as i64;
        let ph = (((t - c as f64) * PHASES as f64) as usize).min(PHASES - 1);
        let wts = &table[ph * 2 * HALF..(ph + 1) * 2 * HALF];
        for k in 0..ch {
            let mut acc = 0f32;
            for (j, w) in wts.iter().enumerate() {
                let n = c - HALF as i64 + 1 + j as i64;
                if n >= 0 && (n as usize) < frames {
                    acc += pcm[n as usize * ch + k] as f32 * w;
                }
            }
            out.push(acc.round().clamp(-32768.0, 32767.0) as i16);
        }
    }
    out
}

/// MIDI rendered with a SoundFont; (stereo PCM, loop start frame from RPG
/// Maker's controller 111)
fn render_midi(sf: &std::sync::Arc<rustysynth::SoundFont>, data: &[u8], rate: u32) -> Result<(Vec<i16>, usize), String> {
    let midi = std::sync::Arc::new(rustysynth::MidiFile::new(&mut std::io::Cursor::new(data)).map_err(|e| format!("{:?}", e))?);
    let settings = rustysynth::SynthesizerSettings::new(rate as i32);
    let synth = rustysynth::Synthesizer::new(sf, &settings).map_err(|e| format!("{:?}", e))?;
    let mut seq = rustysynth::MidiFileSequencer::new(synth);
    seq.play(&midi, false);
    let block = 1024usize;
    let (mut l, mut r) = (vec![0f32; block], vec![0f32; block]);
    let mut pcm = Vec::new();
    let (mut tail, mut quiet) = (0usize, 0usize);
    let max = rate as usize * 60 * 30; // (at most 30 minutes)
    while pcm.len() / 2 < max {
        seq.render(&mut l, &mut r);
        let loud = l.iter().chain(r.iter()).any(|v| v.abs() > 1e-4);
        for i in 0..block {
            pcm.push((l[i] * 32767.0).round().clamp(-32768.0, 32767.0) as i16);
            pcm.push((r[i] * 32767.0).round().clamp(-32768.0, 32767.0) as i16);
        }
        if seq.end_of_sequence() {
            // (the notes ringing out, at most 3 s)
            tail += block;
            quiet = if loud { 0 } else { quiet + block };
            if quiet >= block * 2 || tail > rate as usize * 3 {
                break;
            }
        }
    }
    let loop_ms = midi_loop_ms(data);
    let loop_start = loop_ms.map(|ms| (ms * rate as f64 / 1000.0) as usize).unwrap_or(0);
    Ok((pcm, loop_start))
}

/// the time (ms) of the first controller 111 (RPG Maker's loop point)
fn midi_loop_ms(d: &[u8]) -> Option<f64> {
    if d.get(..4)? != b"MThd" {
        return None;
    }
    let be16 = |o: usize| d.get(o..o + 2).map(|b| u16::from_be_bytes([b[0], b[1]]));
    let be32 = |o: usize| d.get(o..o + 4).map(|b| u32::from_be_bytes([b[0], b[1], b[2], b[3]]));
    let ntracks = be16(10)? as usize;
    let division = be16(12)?;
    if division & 0x8000 != 0 {
        return None; // (SMPTE time: rare)
    }
    let mut tempos: Vec<(u64, u32)> = vec![(0, 500_000)];
    let mut loops = Vec::new();
    let mut p = 8 + be32(4)? as usize;
    for _ in 0..ntracks {
        if d.get(p..p + 4)? != b"MTrk" {
            break;
        }
        let len = be32(p + 4)? as usize;
        let (mut q, end) = (p + 8, (p + 8 + len).min(d.len()));
        let (mut tick, mut status) = (0u64, 0u8);
        let var = |q: &mut usize| -> Option<u64> {
            let mut v = 0u64;
            loop {
                let b = *d.get(*q)?;
                *q += 1;
                v = v << 7 | (b & 0x7f) as u64;
                if b & 0x80 == 0 {
                    return Some(v);
                }
            }
        };
        while q < end {
            tick += var(&mut q)?;
            let mut b = *d.get(q)?;
            if b & 0x80 != 0 {
                status = b;
                q += 1;
                b = *d.get(q).unwrap_or(&0);
            } else if status == 0 {
                break;
            }
            match status {
                0xff => {
                    let kind = *d.get(q)?;
                    q += 1;
                    let n = var(&mut q)? as usize;
                    if kind == 0x51 && n == 3 {
                        let t = (*d.get(q)? as u32) << 16 | (*d.get(q + 1)? as u32) << 8 | *d.get(q + 2)? as u32;
                        tempos.push((tick, t));
                    }
                    q += n;
                    status = 0;
                }
                0xf0 | 0xf7 => {
                    let n = var(&mut q)? as usize;
                    q += n;
                    status = 0;
                }
                s => {
                    let n = match s & 0xf0 { 0xc0 | 0xd0 => 1, _ => 2 };
                    if s & 0xf0 == 0xb0 && b == 111 {
                        loops.push(tick);
                    }
                    q += n;
                }
            }
        }
        p = 8 + p + len;
    }
    let first = *loops.iter().min()?;
    tempos.sort_by_key(|t| t.0);
    let (mut ms, mut last_tick, mut tempo) = (0f64, 0u64, 500_000u32);
    for &(t, v) in &tempos {
        if t >= first {
            break;
        }
        ms += (t - last_tick) as f64 * tempo as f64 / division as f64 / 1000.0;
        last_tick = t;
        tempo = v;
    }
    ms += (first - last_tick) as f64 * tempo as f64 / division as f64 / 1000.0;
    Some(ms)
}

/// .mkxa: "MKXA", u32 version (1), rate, channels, frames (per channel),
/// loop start, loop end (0: none / the end); per channel 16 s16
/// coefficients (8 predictor pairs, 1/2048) and the decoder's history
/// before the frame with the loop start (s16 hist1, hist2); then DSP-ADPCM
/// frames of 14 samples (8 bytes: predictor << 4 | scale, 14 nibbles), the
/// channels' frames interleaved. Little endian. (ports/3ds/tools/mkxa.c)
fn encode_mkxa(pcm: &[i16], rate: u32, ch: usize, loop_start: usize, loop_end: usize) -> Vec<u8> {
    let frames = pcm.len() / ch;
    let loop_start = if loop_start >= frames { 0 } else { loop_start };
    let loop_end = if loop_end <= loop_start || loop_end > frames { 0 } else { loop_end };
    let nf = (frames + 13) / 14;
    let mut data = vec![0u8; nf * ch * 8];
    let mut coefs = vec![[0i16; 16]; ch];
    let mut hist = vec![[0i16; 2]; ch];
    for c in 0..ch {
        let mut chan: Vec<i16> = (0..frames).map(|i| pcm[i * ch + c]).collect();
        coefs[c] = find_coefs(&chan);
        chan.extend_from_slice(&[0; 14]);
        let (mut h1, mut h2) = (0i16, 0i16);
        let loop_frame = loop_start / 14;
        for f in 0..nf {
            if f == loop_frame {
                hist[c] = [h1, h2];
            }
            let left = frames - f * 14;
            let at = (f * ch + c) * 8;
            encode_frame(&chan[f * 14..f * 14 + 14], left.min(14), &coefs[c], &mut h1, &mut h2, &mut data[at..at + 8]);
        }
    }
    let mut out = b"MKXA".to_vec();
    for v in [1u32, rate, ch as u32, frames as u32, loop_start as u32, loop_end as u32] {
        out.extend_from_slice(&v.to_le_bytes());
    }
    for c in 0..ch {
        for k in 0..16 {
            out.extend_from_slice(&coefs[c][k].to_le_bytes());
        }
        out.extend_from_slice(&hist[c][0].to_le_bytes());
        out.extend_from_slice(&hist[c][1].to_le_bytes());
    }
    out.extend_from_slice(&data);
    out
}

fn clamp16(v: i64) -> i16 {
    v.clamp(-32768, 32767) as i16
}

/// the 8 predictors: least squares of each 14-sample frame, grouped into 8
/// (k-means)
fn find_coefs(x: &[i16]) -> [i16; 16] {
    let n = x.len();
    let nf = (n + 13) / 14;
    let mut a = vec![[0f64; 2]; nf];
    let mut w = vec![0f64; nf];
    for f in 0..nf {
        let (mut r11, mut r12, mut r22, mut r01, mut r02) = (0f64, 0f64, 0f64, 0f64, 0f64);
        for i in f * 14..(f * 14 + 14).min(n) {
            let s0 = x[i] as f64;
            let s1 = if i > 0 { x[i - 1] as f64 } else { 0.0 };
            let s2 = if i > 1 { x[i - 2] as f64 } else { 0.0 };
            r11 += s1 * s1;
            r12 += s1 * s2;
            r22 += s2 * s2;
            r01 += s0 * s1;
            r02 += s0 * s2;
        }
        let det = r11 * r22 - r12 * r12;
        let (mut a1, mut a2) = (0.0, 0.0);
        if det.abs() > 1e-3 {
            a1 = (r01 * r22 - r02 * r12) / det;
            a2 = (r02 * r11 - r01 * r12) / det;
        } else if r11 > 1e-3 {
            a1 = r01 / r11;
        }
        a[f] = [a1.clamp(-1.99, 1.99), a2.clamp(-0.99, 0.99)];
        w[f] = r11 + 1.0;
    }
    let mut c = [[0.0, 0.0], [1.0, 0.0], [1.9, -0.95], [1.5, -0.6], [1.2, -0.3], [0.5, 0.0], [1.75, -0.8], [0.9, -0.2]];
    for _ in 0..16 {
        if nf == 0 {
            break;
        }
        let mut sum = [[0f64; 2]; 8];
        let mut wsum = [0f64; 8];
        for f in 0..nf {
            let mut best = 0;
            let mut bd = f64::MAX;
            for k in 0..8 {
                let (d1, d2) = (a[f][0] - c[k][0], a[f][1] - c[k][1]);
                let d = d1 * d1 + d2 * d2;
                if d < bd {
                    bd = d;
                    best = k;
                }
            }
            let ww = w[f].ln();
            sum[best][0] += a[f][0] * ww;
            sum[best][1] += a[f][1] * ww;
            wsum[best] += ww;
        }
        for k in 1..8 {
            // (predictor 0 stays "none")
            if wsum[k] > 0.0 {
                c[k] = [sum[k][0] / wsum[k], sum[k][1] / wsum[k]];
            }
        }
    }
    let mut out = [0i16; 16];
    for k in 0..8 {
        out[k * 2] = clamp16((c[k][0] * 2048.0).round() as i64);
        out[k * 2 + 1] = clamp16((c[k][1] * 2048.0).round() as i64);
    }
    out
}

/// one frame: the predictor and scale with the least error (all tried)
fn encode_frame(x: &[i16], count: usize, coefs: &[i16; 16], h1: &mut i16, h2: &mut i16, out: &mut [u8]) {
    let mut best_err = f64::MAX;
    let mut best = [0u8; 8];
    let (mut bh1, mut bh2) = (*h1, *h2);
    for p in 0..8 {
        let (c1, c2) = (coefs[p * 2] as i64, coefs[p * 2 + 1] as i64);
        for scale in 0..=12 {
            let (mut s1, mut s2) = (*h1 as i64, *h2 as i64);
            let mut err = 0f64;
            let mut buf = [0u8; 8];
            buf[0] = (p << 4 | scale) as u8;
            for i in 0..14 {
                let sample = if i < count { x[i] as i64 } else { 0 };
                let pred = c1 * s1 + c2 * s2;
                let q = (sample as f64 * 2048.0 - pred as f64 - 1024.0) / (2048i64 << scale) as f64;
                let nib = (q.round() as i64).clamp(-8, 7);
                let dec = clamp16((((nib << scale) << 11) + 1024 + pred) >> 11) as i64;
                if i < count {
                    let e = (dec - sample) as f64;
                    err += e * e;
                }
                s2 = s1;
                s1 = dec;
                buf[1 + i / 2] |= ((nib & 15) as u8) << (if i & 1 == 1 { 0 } else { 4 });
            }
            if err < best_err {
                best_err = err;
                best = buf;
                bh1 = s1 as i16;
                bh2 = s2 as i16;
            }
        }
    }
    out.copy_from_slice(&best);
    *h1 = bh1;
    *h2 = bh2;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn formats() {
        assert_eq!(exact_format(&[10, 20, 30, 255]), 1);
        assert_eq!(exact_format(&[17, 17, 17, 255]), 5);
        assert_eq!(exact_format(&[0, 0, 0, 34]), 7);
        assert_eq!(exact_format(&[10, 20, 30, 128]), 0);
    }

    #[test]
    fn adpcm_round_trip_is_close() {
        // a sine: decoded again, close to the original
        let pcm: Vec<i16> = (0..2000).map(|i| ((i as f64 * 0.05).sin() * 12000.0) as i16).collect();
        let m = encode_mkxa(&pcm, 22050, 1, 0, 0);
        assert_eq!(&m[..4], b"MKXA");
        let coefs: Vec<i64> = (0..16).map(|k| i16::from_le_bytes([m[28 + k * 2], m[29 + k * 2]]) as i64).collect();
        let data = &m[28 + 36..];
        let (mut h1, mut h2) = (0i64, 0i64);
        let mut worst = 0i64;
        for f in 0..(2000 + 13) / 14 {
            let fr = &data[f * 8..f * 8 + 8];
            let (p, scale) = ((fr[0] >> 4) as usize, fr[0] & 15);
            for i in 0..14 {
                let byte = fr[1 + i / 2];
                let mut nib = (if i & 1 == 1 { byte & 15 } else { byte >> 4 }) as i64;
                if nib >= 8 {
                    nib -= 16;
                }
                let pred = coefs[p * 2] * h1 + coefs[p * 2 + 1] * h2;
                let dec = clamp16((((nib << scale) << 11) + 1024 + pred) >> 11) as i64;
                h2 = h1;
                h1 = dec;
                let idx = f * 14 + i;
                if idx < 2000 {
                    worst = worst.max((dec - pcm[idx] as i64).abs());
                }
            }
        }
        assert!(worst < 600, "worst error {}", worst);
    }

    #[test]
    fn midi_loop_point() {
        // format 0, 96 ticks per quarter, 120 bpm: CC111 at tick 192 = 1000 ms
        let mut t = vec![0x00, 0xb0, 111, 0, 0x81, 0x40, 0xb0, 111, 0, 0x00, 0xff, 0x2f, 0x00];
        t[4] = 0x81;
        t[5] = 0x40; // (delta 192)
        let mut d = b"MThd".to_vec();
        d.extend_from_slice(&[0, 0, 0, 6, 0, 0, 0, 1, 0, 96]);
        d.extend_from_slice(b"MTrk");
        d.extend_from_slice(&(t.len() as u32).to_be_bytes());
        d.extend_from_slice(&t);
        assert_eq!(midi_loop_ms(&d), Some(0.0));
    }

    #[test]
    fn smdh_layout() {
        let s = smdh("Test", "desc", "pub", &(2, 2, vec![255; 16]));
        assert_eq!(s.len(), 0x36C0);
        assert_eq!(&s[..4], b"SMDH");
        assert_eq!(&s[8..10], &[b'T', 0]);
    }
}
