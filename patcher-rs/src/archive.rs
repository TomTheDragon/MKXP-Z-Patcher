//! RGSS encrypted archives: RGSSAD v1 (`.rgssad`, `.rgss2a`) and v3 (`.rgss3a`).

use std::collections::HashMap;
use std::path::Path;
use std::sync::Arc;

#[derive(Clone, Copy)]
struct Entry {
    offset: usize,
    size: usize,
    key: u32,
}

#[derive(Clone)]
pub struct Archive {
    data: Arc<Vec<u8>>,
    /// lowercase path with forward slashes -> entry
    entries: Arc<HashMap<String, Entry>>,
}

fn u32le(d: &[u8], p: usize) -> Option<u32> {
    d.get(p..p + 4).map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
}

fn advance(k: u32) -> u32 {
    k.wrapping_mul(7).wrapping_add(3)
}

impl Archive {
    pub fn open(path: &Path) -> Result<Archive, String> {
        let d = std::fs::read(path).map_err(|e| format!("{}: {}", path.display(), e))?;
        if d.len() < 8 || &d[..7] != b"RGSSAD\0" {
            return Err("not an RGSS archive".into());
        }
        let mut entries = HashMap::new();
        let bad = || "damaged RGSS archive".to_string();
        match d[7] {
            1 => {
                let (mut p, mut key) = (8usize, 0xDEADCAFEu32);
                while p + 4 <= d.len() {
                    let n = (u32le(&d, p).ok_or_else(bad)? ^ key) as usize;
                    p += 4;
                    key = advance(key);
                    let raw = d.get(p..p + n).ok_or_else(bad)?;
                    let mut name = Vec::with_capacity(n);
                    for b in raw {
                        name.push(b ^ (key & 0xff) as u8);
                        key = advance(key);
                    }
                    p += n;
                    let size = (u32le(&d, p).ok_or_else(bad)? ^ key) as usize;
                    p += 4;
                    key = advance(key);
                    if p + size > d.len() {
                        return Err(bad());
                    }
                    entries.insert(norm(&name), Entry { offset: p, size, key });
                    p += size;
                }
            }
            3 => {
                let base = u32le(&d, 8).ok_or_else(bad)?.wrapping_mul(9).wrapping_add(3);
                let kb = base.to_le_bytes();
                let mut p = 12usize;
                while p + 16 <= d.len() {
                    let f = |i: usize| u32le(&d, p + i).map(|v| v ^ base);
                    let (off, size, fkey, n) = (f(0).ok_or_else(bad)?, f(4).ok_or_else(bad)?,
                                                f(8).ok_or_else(bad)?, f(12).ok_or_else(bad)? as usize);
                    if off == 0 {
                        break;
                    }
                    let raw = d.get(p + 16..p + 16 + n).ok_or_else(bad)?;
                    let name: Vec<u8> = raw.iter().enumerate().map(|(i, b)| b ^ kb[i % 4]).collect();
                    p += 16 + n;
                    if off as usize + size as usize > d.len() {
                        return Err(bad());
                    }
                    entries.insert(norm(&name), Entry { offset: off as usize, size: size as usize, key: fkey });
                }
            }
            v => return Err(format!("unsupported RGSS archive version {}", v)),
        }
        Ok(Archive { data: Arc::new(d), entries: Arc::new(entries) })
    }

    pub fn names(&self) -> impl Iterator<Item = &String> {
        self.entries.keys()
    }

    /// Decrypted contents of `name` (case-insensitive, `/` or `\`).
    pub fn read(&self, name: &str) -> Option<Vec<u8>> {
        let e = *self.entries.get(&norm(name.as_bytes()))?;
        let src = &self.data[e.offset..e.offset + e.size];
        let mut out = Vec::with_capacity(e.size + 4);
        let mut key = e.key;
        for chunk in src.chunks(4) {
            let mut w = [0u8; 4];
            w[..chunk.len()].copy_from_slice(chunk);
            let v = u32::from_le_bytes(w) ^ key;
            out.extend_from_slice(&v.to_le_bytes()[..chunk.len()]);
            key = advance(key);
        }
        Some(out)
    }
}

fn norm(name: &[u8]) -> String {
    String::from_utf8_lossy(name).replace('\\', "/").to_lowercase()
}
