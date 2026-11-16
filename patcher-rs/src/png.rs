//! A minimal PNG writer (RGBA, uncompressed deflate blocks): enough for small
//! images such as the game's icon, without a zlib dependency.

fn crc32(data: &[u8]) -> u32 {
    let mut crc = 0xffff_ffffu32;
    for &b in data {
        crc ^= b as u32;
        for _ in 0..8 {
            crc = if crc & 1 != 0 { (crc >> 1) ^ 0xedb8_8320 } else { crc >> 1 };
        }
    }
    !crc
}

fn adler32(data: &[u8]) -> u32 {
    let (mut a, mut b) = (1u32, 0u32);
    for &x in data {
        a = (a + x as u32) % 65521;
        b = (b + a) % 65521;
    }
    (b << 16) | a
}

fn chunk(out: &mut Vec<u8>, kind: &[u8; 4], data: &[u8]) {
    out.extend_from_slice(&(data.len() as u32).to_be_bytes());
    let start = out.len();
    out.extend_from_slice(kind);
    out.extend_from_slice(data);
    let crc = crc32(&out[start..]);
    out.extend_from_slice(&crc.to_be_bytes());
}

/// zlib stream made of stored (uncompressed) blocks
fn zlib_stored(data: &[u8]) -> Vec<u8> {
    let mut out = vec![0x78, 0x01];
    let mut blocks = data.chunks(65535).peekable();
    if blocks.peek().is_none() {
        out.extend_from_slice(&[1, 0, 0, 0xff, 0xff]);
    }
    while let Some(b) = blocks.next() {
        out.push(if blocks.peek().is_none() { 1 } else { 0 });
        let n = b.len() as u16;
        out.extend_from_slice(&n.to_le_bytes());
        out.extend_from_slice(&(!n).to_le_bytes());
        out.extend_from_slice(b);
    }
    out.extend_from_slice(&adler32(data).to_be_bytes());
    out
}

/// Encode `rgba` (w*h*4 bytes, top row first) as a PNG file.
pub fn encode_rgba(w: u32, h: u32, rgba: &[u8]) -> Vec<u8> {
    assert_eq!(rgba.len(), (w * h * 4) as usize);
    let mut raw = Vec::with_capacity(rgba.len() + h as usize);
    for row in rgba.chunks((w * 4) as usize) {
        raw.push(0); // filter: none
        raw.extend_from_slice(row);
    }
    let mut out = b"\x89PNG\r\n\x1a\n".to_vec();
    let mut ihdr = Vec::new();
    ihdr.extend_from_slice(&w.to_be_bytes());
    ihdr.extend_from_slice(&h.to_be_bytes());
    ihdr.extend_from_slice(&[8, 6, 0, 0, 0]); // 8 bit, RGBA
    chunk(&mut out, b"IHDR", &ihdr);
    chunk(&mut out, b"IDAT", &zlib_stored(&raw));
    chunk(&mut out, b"IEND", &[]);
    out
}

#[cfg(test)]
mod tests {
    #[test]
    fn known_checksums() {
        assert_eq!(super::crc32(b"IEND"), 0xae42_6082);
        assert_eq!(super::adler32(b"Wikipedia"), 0x11e6_0398);
    }

    #[test]
    fn decodes_with_inflate() {
        let px: Vec<u8> = (0..3 * 2 * 4).map(|i| i as u8).collect();
        let png = super::encode_rgba(3, 2, &px);
        assert!(png.starts_with(b"\x89PNG"));
        // IDAT payload inflates back to the filtered rows
        let idat = png.windows(4).position(|w| w == b"IDAT").unwrap();
        let len = u32::from_be_bytes(png[idat - 4..idat].try_into().unwrap()) as usize;
        let z = &png[idat + 4..idat + 4 + len];
        let raw = crate::game::inflate(z).unwrap();
        assert_eq!(raw.len(), 2 * (1 + 12));
        assert_eq!(&raw[1..13], &px[0..12]);
    }
}
