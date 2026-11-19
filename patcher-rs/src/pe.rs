//! Minimal PE (Windows .exe) reading and writing:
//! - copy the game's icon from Game.exe into our launcher,
//! - list a program's imported DLLs.

fn u16at(d: &[u8], o: usize) -> Option<u16> {
    d.get(o..o + 2).map(|b| u16::from_le_bytes([b[0], b[1]]))
}
fn u32at(d: &[u8], o: usize) -> Option<u32> {
    d.get(o..o + 4).map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
}
fn put16(d: &mut [u8], o: usize, v: u16) {
    d[o..o + 2].copy_from_slice(&v.to_le_bytes());
}
fn put32(d: &mut [u8], o: usize, v: u32) {
    d[o..o + 4].copy_from_slice(&v.to_le_bytes());
}
fn align(v: usize, a: usize) -> usize {
    if a == 0 { v } else { v.div_ceil(a) * a }
}

#[derive(Clone, Copy)]
struct Section {
    va: usize,
    vsize: usize,
    raw_off: usize,
    raw_size: usize,
}

struct Pe {
    sections: Vec<Section>,
    /// offset of the optional header
    opt: usize,
    /// offset of the data directory array
    dirs: usize,
    /// offset of the section table
    sec_table: usize,
    nsec: usize,
}

impl Pe {
    fn parse(d: &[u8]) -> Option<Pe> {
        if d.get(..2)? != b"MZ" {
            return None;
        }
        let pe = u32at(d, 0x3c)? as usize;
        if d.get(pe..pe + 4)? != b"PE\0\0" {
            return None;
        }
        let coff = pe + 4;
        let nsec = u16at(d, coff + 2)? as usize;
        let opt_size = u16at(d, coff + 16)? as usize;
        let opt = coff + 20;
        let dirs = opt + match u16at(d, opt)? { 0x10b => 96, 0x20b => 112, _ => return None };
        let sec_table = opt + opt_size;
        let mut sections = Vec::with_capacity(nsec);
        for i in 0..nsec {
            let s = sec_table + i * 40;
            sections.push(Section {
                vsize: u32at(d, s + 8)? as usize,
                va: u32at(d, s + 12)? as usize,
                raw_size: u32at(d, s + 16)? as usize,
                raw_off: u32at(d, s + 20)? as usize,
            });
        }
        Some(Pe { sections, opt, dirs, sec_table, nsec })
    }

    fn dir(&self, d: &[u8], i: usize) -> Option<(usize, usize)> {
        Some((u32at(d, self.dirs + i * 8)? as usize, u32at(d, self.dirs + i * 8 + 4)? as usize))
    }

    fn off(&self, rva: usize) -> Option<usize> {
        self.sections.iter()
            .find(|s| rva >= s.va && rva < s.va + s.vsize.max(s.raw_size))
            .map(|s| rva - s.va + s.raw_off)
    }
}

fn cstr(d: &[u8], o: usize) -> Option<String> {
    let end = o + d.get(o..)?.iter().position(|b| *b == 0)?;
    Some(String::from_utf8_lossy(&d[o..end]).into_owned())
}

/// Names of the DLLs a program imports directly.
pub fn imported_dlls(d: &[u8]) -> Vec<String> {
    let mut out = Vec::new();
    let Some(pe) = Pe::parse(d) else { return out };
    let Some((rva, _)) = pe.dir(d, 1).filter(|(r, _)| *r != 0) else { return out };
    let Some(mut o) = pe.off(rva) else { return out };
    while let (Some(name_rva), Some(first)) = (u32at(d, o + 12), u32at(d, o)) {
        if name_rva == 0 && first == 0 {
            break;
        }
        if let Some(n) = pe.off(name_rva as usize).and_then(|x| cstr(d, x)) {
            out.push(n);
        }
        o += 20;
    }
    out
}

/// The functions a program imports: (DLL, function name); by ordinal: "#<n>".
pub fn imported_functions(d: &[u8]) -> Vec<(String, String)> {
    let mut out = Vec::new();
    let Some(pe) = Pe::parse(d) else { return out };
    let wide = u16at(d, pe.opt) == Some(0x20b);
    let Some((rva, _)) = pe.dir(d, 1).filter(|(r, _)| *r != 0) else { return out };
    let Some(mut o) = pe.off(rva) else { return out };
    while let (Some(name_rva), Some(lookup), Some(first)) = (u32at(d, o + 12), u32at(d, o), u32at(d, o + 16)) {
        if name_rva == 0 && first == 0 {
            break;
        }
        let dll = pe.off(name_rva as usize).and_then(|x| cstr(d, x)).unwrap_or_default();
        // the lookup table, or (old linkers) the address table before binding
        let table = if lookup != 0 { lookup } else { first };
        if let Some(mut t) = pe.off(table as usize) {
            loop {
                let (entry, by_ordinal) = if wide {
                    let Some(v) = d.get(t..t + 8).map(|b| u64::from_le_bytes(b.try_into().unwrap())) else { break };
                    (v & 0x7fff_ffff_ffff_ffff, v >> 63 == 1)
                } else {
                    let Some(v) = u32at(d, t) else { break };
                    ((v & 0x7fff_ffff) as u64, v >> 31 == 1)
                };
                if entry == 0 && !by_ordinal {
                    break;
                }
                let name = if by_ordinal {
                    format!("#{}", entry & 0xffff)
                } else {
                    pe.off(entry as usize + 2).and_then(|x| cstr(d, x)).unwrap_or_default()
                };
                out.push((dll.clone(), name));
                t += if wide { 8 } else { 4 };
            }
        }
        o += 20;
    }
    out
}

// ------------------------------------------------------------------ icons

/// An icon: group directory entries plus the image data (BMP/PNG) of each.
pub struct Icon {
    /// width, height, colors, reserved, planes, bitcount (as in GRPICONDIRENTRY)
    entries: Vec<[u16; 6]>,
    images: Vec<Vec<u8>>,
}

const RT_ICON: u32 = 3;
const RT_GROUP_ICON: u32 = 14;

/// Read an .ico file.
pub fn icon_from_ico(d: &[u8]) -> Option<Icon> {
    if u16at(d, 0)? != 0 || u16at(d, 2)? != 1 {
        return None;
    }
    let n = u16at(d, 4)? as usize;
    let (mut entries, mut images) = (Vec::new(), Vec::new());
    for i in 0..n {
        let e = 6 + i * 16;
        let size = u32at(d, e + 8)? as usize;
        let off = u32at(d, e + 12)? as usize;
        entries.push([d[e] as u16, d[e + 1] as u16, d[e + 2] as u16, d[e + 3] as u16, u16at(d, e + 4)?, u16at(d, e + 6)?]);
        images.push(d.get(off..off + size)?.to_vec());
    }
    (!images.is_empty()).then_some(Icon { entries, images })
}

impl Icon {
    /// The largest image of the icon as a PNG file (for mkxp-z's "iconPath").
    pub fn to_png(&self) -> Option<Vec<u8>> {
        let mut best: Option<(u64, Vec<u8>)> = None;
        for img in &self.images {
            let Some((w, h, png)) = image_png(img) else { continue };
            let score = (w as u64 * h as u64) << 8;
            if best.as_ref().map_or(true, |(s, _)| score > *s) {
                best = Some((score, png));
            }
        }
        best.map(|(_, png)| png)
    }
}

/// One icon image (PNG, or a DIB as stored in .ico files) as (w, h, PNG).
fn image_png(img: &[u8]) -> Option<(u32, u32, Vec<u8>)> {
    if img.starts_with(b"\x89PNG\r\n\x1a\n") {
        let w = u32::from_be_bytes(img.get(16..20)?.try_into().ok()?);
        let h = u32::from_be_bytes(img.get(20..24)?.try_into().ok()?);
        return Some((w, h, img.to_vec()));
    }
    let (w, h, rgba) = dib_to_rgba(img)?;
    Some((w, h, crate::png::encode_rgba(w, h, &rgba)))
}

/// Decode an icon DIB (BITMAPINFOHEADER, XOR image + AND mask) to RGBA.
fn dib_to_rgba(d: &[u8]) -> Option<(u32, u32, Vec<u8>)> {
    let hsize = u32at(d, 0)? as usize;
    let w = u32at(d, 4)? as i32;
    let h2 = u32at(d, 8)? as i32;
    let bpp = u16at(d, 14)? as usize;
    let compression = u32at(d, 16)?;
    let clr_used = u32at(d, 32)? as usize;
    if w <= 0 || w > 1024 || h2 <= 0 || compression != 0 || hsize < 40 {
        return None;
    }
    let (w, h) = (w as usize, (h2 / 2).max(1) as usize);
    let pal_n = if bpp <= 8 { if clr_used != 0 { clr_used } else { 1 << bpp } } else { 0 };
    let pal = d.get(hsize..hsize + pal_n * 4)?;
    let xor_off = hsize + pal_n * 4;
    let stride = (w * bpp + 31) / 32 * 4;
    let and_off = xor_off + stride * h;
    let and_stride = (w + 31) / 32 * 4;
    let have_mask = d.len() >= and_off + and_stride * h;
    let mut out = vec![0u8; w * h * 4];
    let mut any_alpha = false;
    for y in 0..h {
        let row = d.get(xor_off + (h - 1 - y) * stride..xor_off + (h - y) * stride)?;
        for x in 0..w {
            let (b, g, r, a) = match bpp {
                32 => (row[x * 4], row[x * 4 + 1], row[x * 4 + 2], row[x * 4 + 3]),
                24 => (row[x * 3], row[x * 3 + 1], row[x * 3 + 2], 255),
                1 | 4 | 8 => {
                    let bit = x * bpp;
                    let i = (row[bit / 8] >> (8 - bpp - bit % 8)) & ((1u16 << bpp) - 1) as u8;
                    let c = pal.get(i as usize * 4..i as usize * 4 + 3)?;
                    (c[0], c[1], c[2], 255)
                }
                _ => return None,
            };
            if bpp == 32 && a != 0 {
                any_alpha = true;
            }
            out[(y * w + x) * 4..(y * w + x) * 4 + 4].copy_from_slice(&[r, g, b, a]);
        }
    }
    // without an alpha channel (or an empty one), the AND mask says what's transparent
    if bpp != 32 || !any_alpha {
        for y in 0..h {
            for x in 0..w {
                let transparent = have_mask && {
                    let row = and_off + (h - 1 - y) * and_stride;
                    d[row + x / 8] & (0x80 >> (x % 8)) != 0
                };
                out[(y * w + x) * 4 + 3] = if transparent { 0 } else { 255 };
            }
        }
    }
    Some((w as u32, h as u32, out))
}

/// Walk one level of a resource directory: (id or None for named, offset field) pairs.
fn res_entries(d: &[u8], base: usize, dir: usize) -> Vec<(Option<u32>, u32)> {
    let mut v = Vec::new();
    let (Some(named), Some(ids)) = (u16at(d, base + dir + 12), u16at(d, base + dir + 14)) else { return v };
    for i in 0..(named as usize + ids as usize) {
        let e = base + dir + 16 + i * 8;
        if let (Some(name), Some(off)) = (u32at(d, e), u32at(d, e + 4)) {
            v.push(((name & 0x8000_0000 == 0).then_some(name), off));
        }
    }
    v
}

/// The first data blob below a directory entry (descending into the first child).
fn res_data<'a>(d: &'a [u8], pe: &Pe, base: usize, mut off: u32) -> Option<&'a [u8]> {
    for _ in 0..4 {
        if off & 0x8000_0000 == 0 {
            let e = base + off as usize;
            let rva = u32at(d, e)? as usize;
            let size = u32at(d, e + 4)? as usize;
            let o = pe.off(rva)?;
            return d.get(o..o + size);
        }
        off = res_entries(d, base, (off & 0x7fff_ffff) as usize).first()?.1;
    }
    None
}

/// The main (first) icon of an .exe, e.g. Game.exe.
pub fn icon_from_exe(d: &[u8]) -> Option<Icon> {
    let pe = Pe::parse(d)?;
    let (rva, _) = pe.dir(d, 2).filter(|(r, _)| *r != 0)?;
    let base = pe.off(rva)?;
    let types = res_entries(d, base, 0);
    let sub = |t: u32| types.iter().find(|(id, _)| *id == Some(t)).map(|(_, o)| (o & 0x7fff_ffff) as usize);
    let (groups, icons) = (sub(RT_GROUP_ICON)?, sub(RT_ICON)?);
    let group = res_data(d, &pe, base, res_entries(d, base, groups).first()?.1)?;
    let icon_list = res_entries(d, base, icons);
    let n = u16at(group, 4)? as usize;
    let (mut entries, mut images) = (Vec::new(), Vec::new());
    for i in 0..n {
        let e = 6 + i * 14;
        let id = u16at(group, e + 12)? as u32;
        let Some((_, off)) = icon_list.iter().find(|(x, _)| *x == Some(id)) else { continue };
        let img = res_data(d, &pe, base, *off)?;
        entries.push([group[e] as u16, group[e + 1] as u16, group[e + 2] as u16, group[e + 3] as u16,
                      u16at(group, e + 4)?, u16at(group, e + 6)?]);
        images.push(img.to_vec());
    }
    (!images.is_empty()).then_some(Icon { entries, images })
}

/// A resource name: numeric id or UTF-16 string.
#[derive(Clone, PartialEq, Eq, PartialOrd, Ord, Debug)]
enum ResName {
    // named entries come first in a directory, then ids (both sorted)
    Name(Vec<u16>),
    Id(u32),
}

/// One resource: type, name, language, data.
struct Res {
    kind: ResName,
    name: ResName,
    lang: u32,
    data: Vec<u8>,
}

fn res_name(d: &[u8], base: usize, field: u32) -> Option<ResName> {
    if field & 0x8000_0000 == 0 {
        return Some(ResName::Id(field));
    }
    let o = base + (field & 0x7fff_ffff) as usize;
    let n = u16at(d, o)? as usize;
    (0..n).map(|i| u16at(d, o + 2 + i * 2)).collect::<Option<Vec<u16>>>().map(ResName::Name)
}

/// All resources of a program (empty if it has none).
fn read_resources(d: &[u8], pe: &Pe) -> Option<Vec<Res>> {
    let Some((rva, _)) = pe.dir(d, 2).filter(|(r, _)| *r != 0) else { return Some(Vec::new()) };
    let base = pe.off(rva)?;
    // directory entries with their raw name field
    let entries = |dir: usize| -> Option<Vec<(u32, u32)>> {
        let n = u16at(d, base + dir + 12)? as usize + u16at(d, base + dir + 14)? as usize;
        (0..n).map(|i| {
            let e = base + dir + 16 + i * 8;
            Some((u32at(d, e)?, u32at(d, e + 4)?))
        }).collect()
    };
    let mut out = Vec::new();
    for (tf, toff) in entries(0)? {
        if toff & 0x8000_0000 == 0 { continue; }
        for (nf, noff) in entries((toff & 0x7fff_ffff) as usize)? {
            if noff & 0x8000_0000 == 0 { continue; }
            for (lf, loff) in entries((noff & 0x7fff_ffff) as usize)? {
                if loff & 0x8000_0000 != 0 { continue; }
                let e = base + loff as usize;
                let (drva, size) = (u32at(d, e)? as usize, u32at(d, e + 4)? as usize);
                let o = pe.off(drva)?;
                out.push(Res { kind: res_name(d, base, tf)?, name: res_name(d, base, nf)?,
                               lang: lf, data: d.get(o..o + size)?.to_vec() });
            }
        }
    }
    Some(out)
}

/// The resources with their icons replaced by `icon` (one icon group, named
/// like the program's first one, so that it stays the program's main icon).
fn with_icon_resources(mut res: Vec<Res>, icon: &Icon) -> Vec<Res> {
    let mut groups: Vec<&Res> = res.iter().filter(|r| r.kind == ResName::Id(RT_GROUP_ICON)).collect();
    groups.sort_by(|a, b| a.name.cmp(&b.name));
    let group_name = groups.first().map_or(ResName::Id(1), |g| g.name.clone());
    let lang = groups.first().map_or(0x409, |g| g.lang);
    res.retain(|r| r.kind != ResName::Id(RT_ICON) && r.kind != ResName::Id(RT_GROUP_ICON));

    let n = icon.images.len();
    let mut group = vec![0u8; 6 + n * 14];
    put16(&mut group, 2, 1);
    put16(&mut group, 4, n as u16);
    for (i, e) in icon.entries.iter().enumerate() {
        let o = 6 + i * 14;
        group[o] = e[0] as u8;
        group[o + 1] = e[1] as u8;
        group[o + 2] = e[2] as u8;
        group[o + 3] = e[3] as u8;
        put16(&mut group, o + 4, e[4]);
        put16(&mut group, o + 6, e[5]);
        put32(&mut group, o + 8, icon.images[i].len() as u32);
        put16(&mut group, o + 12, (i + 1) as u16);
        res.push(Res { kind: ResName::Id(RT_ICON), name: ResName::Id((i + 1) as u32), lang,
                       data: icon.images[i].clone() });
    }
    res.push(Res { kind: ResName::Id(RT_GROUP_ICON), name: group_name, lang, data: group });
    res
}

/// Build a resource section (type / name / language tree) to be loaded at `va`.
fn build_rsrc(res: &[Res], va: usize) -> Vec<u8> {
    use std::collections::BTreeMap;
    let mut tree: BTreeMap<&ResName, BTreeMap<&ResName, BTreeMap<u32, &[u8]>>> = BTreeMap::new();
    for r in res {
        tree.entry(&r.kind).or_default().entry(&r.name).or_default().insert(r.lang, &r.data);
    }
    let dir_size = |n: usize| 16 + n * 8;
    // 1st pass: offsets of all directories, then strings, data entries, data
    let mut size = dir_size(tree.len());
    let mut type_dirs = Vec::new();
    let mut name_dirs = Vec::new();
    for names in tree.values() {
        type_dirs.push(size);
        size += dir_size(names.len());
    }
    for names in tree.values() {
        for langs in names.values() {
            name_dirs.push(size);
            size += dir_size(langs.len());
        }
    }
    let mut strings: Vec<(&ResName, usize)> = Vec::new();
    let all_names = tree.keys().copied().chain(tree.values().flat_map(|n| n.keys().copied()));
    for nm in all_names {
        if let ResName::Name(u) = nm {
            if !strings.iter().any(|(x, _)| *x == nm) {
                strings.push((nm, size));
                size += 2 + u.len() * 2;
            }
        }
    }
    size = align(size, 4);
    let count: usize = tree.values().flat_map(|n| n.values()).map(|l| l.len()).sum();
    let data_entries = size;
    size += count * 16;

    let mut out = vec![0u8; size];
    let name_field = |nm: &ResName| -> u32 {
        match nm {
            ResName::Id(id) => *id,
            ResName::Name(_) => 0x8000_0000 | strings.iter().find(|(x, _)| *x == nm).unwrap().1 as u32,
        }
    };
    let header = |out: &mut Vec<u8>, at: usize, keys: &mut dyn Iterator<Item = bool>| {
        let (mut named, mut ids) = (0u16, 0u16);
        for is_named in keys {
            if is_named { named += 1 } else { ids += 1 }
        }
        put16(out, at + 12, named);
        put16(out, at + 14, ids);
    };
    for (nm, at) in &strings {
        if let ResName::Name(u) = nm {
            put16(&mut out, *at, u.len() as u16);
            for (i, c) in u.iter().enumerate() {
                put16(&mut out, at + 2 + i * 2, *c);
            }
        }
    }
    header(&mut out, 0, &mut tree.keys().map(|k| matches!(k, ResName::Name(_))));
    let (mut nd, mut de) = (0usize, 0usize);
    for (ti, (kind, names)) in tree.iter().enumerate() {
        let tdir = type_dirs[ti];
        put32(&mut out, 16 + ti * 8, name_field(kind));
        put32(&mut out, 16 + ti * 8 + 4, 0x8000_0000 | tdir as u32);
        header(&mut out, tdir, &mut names.keys().map(|k| matches!(k, ResName::Name(_))));
        for (ni, (name, langs)) in names.iter().enumerate() {
            let ndir = name_dirs[nd];
            nd += 1;
            put32(&mut out, tdir + 16 + ni * 8, name_field(name));
            put32(&mut out, tdir + 16 + ni * 8 + 4, 0x8000_0000 | ndir as u32);
            header(&mut out, ndir, &mut langs.keys().map(|_| false));
            for (li, (lang, data)) in langs.iter().enumerate() {
                let entry = data_entries + de * 16;
                de += 1;
                put32(&mut out, ndir + 16 + li * 8, *lang);
                put32(&mut out, ndir + 16 + li * 8 + 4, entry as u32);
                let at = align(out.len(), 8);
                out.resize(at, 0);
                put32(&mut out, entry, (va + at) as u32);
                put32(&mut out, entry + 4, data.len() as u32);
                out.extend_from_slice(data);
            }
        }
    }
    out
}

/// Return a copy of `exe` whose icon is `icon`; its other resources (version
/// info, manifest, ...) are kept. The resources are written to a new section
/// at the end (the old one, if any, stays in the file unused).
pub fn with_icon(exe: &[u8], icon: &Icon) -> Result<Vec<u8>, String> {
    let pe = Pe::parse(exe).ok_or("not a PE file")?;
    let res = read_resources(exe, &pe).ok_or("unreadable resources")?;
    let res = with_icon_resources(res, icon);
    let sa = u32at(exe, pe.opt + 32).ok_or("bad header")? as usize;
    let fa = u32at(exe, pe.opt + 36).ok_or("bad header")? as usize;
    let headers = u32at(exe, pe.opt + 60).ok_or("bad header")? as usize;
    let new_hdr = pe.sec_table + pe.nsec * 40;
    let first_raw = pe.sections.iter().filter(|s| s.raw_size > 0).map(|s| s.raw_off).min().unwrap_or(headers);
    if new_hdr + 40 > headers.min(first_raw) || exe[new_hdr..new_hdr + 40].iter().any(|b| *b != 0) {
        return Err("no room for another section header".into());
    }
    if pe.dir(exe, 4).map_or(false, |(r, _)| r != 0) {
        return Err("the program is signed".into());
    }
    let end_va = pe.sections.iter().map(|s| s.va + align(s.vsize.max(s.raw_size), sa)).max().unwrap_or(sa);
    let va = align(end_va, sa);
    let rsrc = build_rsrc(&res, va);
    let raw_off = align(exe.len(), fa);
    let raw_size = align(rsrc.len(), fa);

    let mut out = exe.to_vec();
    out.resize(raw_off, 0);
    out.extend_from_slice(&rsrc);
    out.resize(raw_off + raw_size, 0);
    out[new_hdr..new_hdr + 8].copy_from_slice(b".rsrc\0\0\0");
    put32(&mut out, new_hdr + 8, rsrc.len() as u32);
    put32(&mut out, new_hdr + 12, va as u32);
    put32(&mut out, new_hdr + 16, raw_size as u32);
    put32(&mut out, new_hdr + 20, raw_off as u32);
    put32(&mut out, new_hdr + 36, 0x4000_0040); // initialized data, readable
    let coff = u32at(exe, 0x3c).unwrap() as usize + 4;
    put16(&mut out, coff + 2, (pe.nsec + 1) as u16);
    let init = u32at(&out, pe.opt + 8).unwrap_or(0);
    put32(&mut out, pe.opt + 8, init + raw_size as u32);      // SizeOfInitializedData
    put32(&mut out, pe.opt + 56, align(va + rsrc.len(), sa) as u32); // SizeOfImage
    put32(&mut out, pe.opt + 64, 0);                              // CheckSum (not needed for .exe)
    put32(&mut out, pe.dirs + 16, va as u32);                     // resource directory
    put32(&mut out, pe.dirs + 20, rsrc.len() as u32);
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tiny_ico() -> Vec<u8> {
        // 2 fake images
        let imgs: [&[u8]; 2] = [b"\x89PNG-image-one", b"BMP-image-two!!"];
        let mut d = vec![0, 0, 1, 0, 2, 0];
        let mut off = 6 + 2 * 16;
        for (i, img) in imgs.iter().enumerate() {
            d.extend_from_slice(&[16 * (i as u8 + 1), 16 * (i as u8 + 1), 0, 0, 1, 0, 32, 0]);
            d.extend_from_slice(&(img.len() as u32).to_le_bytes());
            d.extend_from_slice(&(off as u32).to_le_bytes());
            off += img.len();
        }
        for img in imgs {
            d.extend_from_slice(img);
        }
        d
    }

    #[test]
    fn dib_icon_decodes() {
        // 2x2, 24 bit, bottom row first; AND mask makes the top-left pixel transparent
        let mut d = vec![0u8; 40];
        d[0] = 40;
        d[4] = 2;
        d[8] = 4; // height counts XOR + AND
        d[12] = 1;
        d[14] = 24;
        // bottom row: blue, green (+2 padding bytes); top row: red, white
        d.extend_from_slice(&[255, 0, 0, 0, 255, 0, 0, 0]);
        d.extend_from_slice(&[0, 0, 255, 255, 255, 255, 0, 0]);
        // AND mask rows (bottom first), 4 bytes each
        d.extend_from_slice(&[0, 0, 0, 0, 0x80, 0, 0, 0]);
        let (w, h, px) = dib_to_rgba(&d).unwrap();
        assert_eq!((w, h), (2, 2));
        assert_eq!(&px[0..4], &[255, 0, 0, 0]); // red, transparent
        assert_eq!(&px[4..8], &[255, 255, 255, 255]);
        assert_eq!(&px[8..12], &[0, 0, 255, 255]);
        assert_eq!(&px[12..16], &[0, 255, 0, 255]);
    }

    #[test]
    fn ico_round_trip_through_launcher() {
        let launcher = include_bytes!(concat!(env!("OUT_DIR"), "/launcher.exe"));
        if launcher.is_empty() {
            return; // launcher not built in this environment
        }
        let icon = icon_from_ico(&tiny_ico()).unwrap();
        let patched = with_icon(launcher, &icon).unwrap();
        let back = icon_from_exe(&patched).expect("icon readable from patched exe");
        assert_eq!(back.images, icon.images);
        assert_eq!(back.entries, icon.entries);
        // imports survive
        let dlls = imported_dlls(&patched);
        assert!(dlls.iter().any(|d| d.eq_ignore_ascii_case("kernel32.dll")), "{:?}", dlls);
        // again (the program now has resources): replaced, not added
        let other = Icon { entries: vec![icon.entries[0]], images: vec![b"another".to_vec()] };
        let twice = with_icon(&patched, &other).unwrap();
        let back = icon_from_exe(&twice).unwrap();
        assert_eq!(back.images, other.images);
        let pe = Pe::parse(&twice).unwrap();
        let res = read_resources(&twice, &pe).unwrap();
        assert_eq!(res.iter().filter(|r| r.kind == ResName::Id(RT_ICON)).count(), 1);
        assert_eq!(res.iter().filter(|r| r.kind == ResName::Id(RT_GROUP_ICON)).count(), 1);
    }
}

#[cfg(test)]
mod real_exe_tests {
    use super::*;
    /// MKXPZ_TEST_EXES="a.exe:b.exe" cargo test -- --ignored real_game_icons
    #[test]
    #[ignore]
    fn real_game_icons() {
        let launcher = include_bytes!(concat!(env!("OUT_DIR"), "/launcher.exe"));
        for p in std::env::var("MKXPZ_TEST_EXES").unwrap_or_default().split(':').filter(|s| !s.is_empty()) {
            let d = std::fs::read(p).unwrap();
            let icon = icon_from_exe(&d).unwrap_or_else(|| panic!("no icon in {}", p));
            let sizes: Vec<String> = icon.entries.iter().map(|e| format!("{}x{}@{}", e[0], e[1], e[5])).collect();
            let patched = with_icon(launcher, &icon).unwrap();
            let back = icon_from_exe(&patched).unwrap();
            assert_eq!(back.images, icon.images);
            println!("{}: {} images [{}], launcher {} -> {} bytes, imports {:?}", p, icon.images.len(),
                     sizes.join(" "), launcher.len(), patched.len(), imported_dlls(&d));
        }
    }
}
