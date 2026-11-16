//! Ruby Marshal 4.8 reader: just enough for RPG Maker's data files.
//!
//! Object links (`@`) aren't resolved: every object is visited once where it
//! first appears, which is all the text scanner and script reader need, and it
//! means no object table has to be kept.

#[derive(Debug, Clone)]
#[allow(dead_code)] // kept for completeness; the patcher reads only some fields
pub enum Value {
    Nil,
    Bool(bool),
    Int(i64),
    Float(f64),
    /// Raw string bytes (RGSS1/2 strings carry no encoding; RGSS3 ones are UTF-8).
    Str(Vec<u8>),
    Sym(String),
    Array(Vec<Value>),
    Hash(Vec<(Value, Value)>),
    Object { class: String, ivars: Vec<(String, Value)> },
    /// `_dump`ed binary such as Table, Color or Tone. Never text.
    User { class: String },
    /// Reference to an object seen earlier in the stream.
    Link,
}

impl Value {
    pub fn ivar(&self, name: &str) -> Option<&Value> {
        match self {
            Value::Object { ivars, .. } => ivars.iter().find(|(k, _)| k == name).map(|(_, v)| v),
            _ => None,
        }
    }
    pub fn as_int(&self) -> Option<i64> {
        match self {
            Value::Int(i) => Some(*i),
            _ => None,
        }
    }
    pub fn as_bytes(&self) -> Option<&[u8]> {
        match self {
            Value::Str(s) => Some(s),
            _ => None,
        }
    }
}

pub struct Reader<'a> {
    d: &'a [u8],
    pos: usize,
    syms: Vec<String>,
}

pub fn load(d: &[u8]) -> Result<Value, String> {
    if d.len() < 2 || d[0] != 4 || d[1] != 8 {
        return Err("not a Ruby Marshal 4.8 stream".into());
    }
    let mut r = Reader { d, pos: 2, syms: Vec::new() };
    r.value(0)
}

impl<'a> Reader<'a> {
    fn eof(&self) -> String {
        format!("unexpected end of data at {}", self.pos)
    }

    fn byte(&mut self) -> Result<u8, String> {
        let b = *self.d.get(self.pos).ok_or_else(|| self.eof())?;
        self.pos += 1;
        Ok(b)
    }

    fn take(&mut self, n: usize) -> Result<&'a [u8], String> {
        if self.pos + n > self.d.len() {
            return Err(self.eof());
        }
        let s = &self.d[self.pos..self.pos + n];
        self.pos += n;
        Ok(s)
    }

    fn int(&mut self) -> Result<i64, String> {
        let c = self.byte()? as i8 as i64;
        Ok(match c {
            0 => 0,
            5..=127 => c - 5,
            -128..=-5 => c + 5,
            1..=4 => {
                let mut n: i64 = 0;
                for (i, b) in self.take(c as usize)?.iter().enumerate() {
                    n |= (*b as i64) << (8 * i);
                }
                n
            }
            _ => {
                let len = (-c) as usize;
                let mut n: i64 = 0;
                for (i, b) in self.take(len)?.iter().enumerate() {
                    n |= (*b as i64) << (8 * i);
                }
                n - (1i64 << (8 * len))
            }
        })
    }

    fn len(&mut self) -> Result<usize, String> {
        let n = self.int()?;
        if n < 0 || n as usize > self.d.len() {
            return Err(format!("bad length {} at {}", n, self.pos));
        }
        Ok(n as usize)
    }

    fn bytes(&mut self) -> Result<&'a [u8], String> {
        let n = self.len()?;
        self.take(n)
    }

    fn symbol(&mut self) -> Result<String, String> {
        match self.byte()? {
            b':' => {
                let s = String::from_utf8_lossy(self.bytes()?).into_owned();
                self.syms.push(s.clone());
                Ok(s)
            }
            b';' => {
                let i = self.len()?;
                self.syms.get(i).cloned().ok_or_else(|| format!("bad symbol link {}", i))
            }
            t => Err(format!("symbol expected, got {:?} at {}", t as char, self.pos - 1)),
        }
    }

    fn value(&mut self, depth: usize) -> Result<Value, String> {
        if depth > 2000 {
            return Err("nesting too deep".into());
        }
        let t = self.byte()?;
        Ok(match t {
            b'0' => Value::Nil,
            b'T' => Value::Bool(true),
            b'F' => Value::Bool(false),
            b'i' => Value::Int(self.int()?),
            b'"' => Value::Str(self.bytes()?.to_vec()),
            b':' | b';' => {
                self.pos -= 1;
                Value::Sym(self.symbol()?)
            }
            b'@' => {
                self.int()?;
                Value::Link
            }
            b'I' => {
                let v = self.value(depth + 1)?;
                for _ in 0..self.len()? {
                    self.symbol()?;
                    self.value(depth + 1)?;
                }
                v
            }
            b'[' => {
                let n = self.len()?;
                let mut a = Vec::with_capacity(n.min(1 << 16));
                for _ in 0..n {
                    a.push(self.value(depth + 1)?);
                }
                Value::Array(a)
            }
            b'{' | b'}' => {
                let n = self.len()?;
                let mut h = Vec::with_capacity(n.min(1 << 16));
                for _ in 0..n {
                    let k = self.value(depth + 1)?;
                    let v = self.value(depth + 1)?;
                    h.push((k, v));
                }
                if t == b'}' {
                    self.value(depth + 1)?; // default value
                }
                Value::Hash(h)
            }
            b'o' | b'S' => {
                let class = self.symbol()?;
                let n = self.len()?;
                let mut ivars = Vec::with_capacity(n.min(1 << 12));
                for _ in 0..n {
                    let k = self.symbol()?;
                    let k = if t == b'S' { format!("@{}", k) } else { k };
                    ivars.push((k, self.value(depth + 1)?));
                }
                Value::Object { class, ivars }
            }
            b'u' => {
                let class = self.symbol()?;
                self.bytes()?;
                Value::User { class }
            }
            b'U' => {
                let class = self.symbol()?;
                let data = self.value(depth + 1)?;
                Value::Object { class, ivars: vec![("@_data".into(), data)] }
            }
            b'e' | b'C' => {
                self.symbol()?;
                self.value(depth + 1)?
            }
            b'f' => {
                // Ruby 1.8/1.9 append mantissa bytes after a NUL
                let raw = self.bytes()?;
                let s = raw.split(|b| *b == 0).next().unwrap_or(b"");
                let s = String::from_utf8_lossy(s);
                Value::Float(match s.as_ref() {
                    "inf" => f64::INFINITY,
                    "-inf" => f64::NEG_INFINITY,
                    "nan" => f64::NAN,
                    x => x.parse().unwrap_or(0.0),
                })
            }
            b'l' => {
                let sign = self.byte()?;
                let n = self.len()? * 2;
                let digits = self.take(n)?;
                let mut v: i128 = 0;
                for (i, b) in digits.iter().enumerate().take(15) {
                    v |= (*b as i128) << (8 * i);
                }
                let v = if sign == b'-' { -v } else { v };
                Value::Int(v.clamp(i64::MIN as i128, i64::MAX as i128) as i64)
            }
            b'c' | b'm' => Value::Str(self.bytes()?.to_vec()),
            b'/' => {
                let s = self.bytes()?.to_vec();
                self.byte()?; // options
                Value::Str(s)
            }
            other => return Err(format!("unsupported Marshal type {:?} at {}", other as char, self.pos - 1)),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ints() {
        // 0, 1, -1, 122, -123, 123, 256, -257
        let cases: &[(&[u8], i64)] = &[
            (&[0], 0), (&[6], 1), (&[0xfa], -1), (&[0x7f], 122), (&[0x80], -123),
            (&[1, 123], 123), (&[2, 0, 1], 256), (&[0xfe, 0xff, 0xfe], -257),
        ];
        for (bytes, want) in cases {
            let mut d = vec![4, 8, b'i'];
            d.extend_from_slice(bytes);
            assert_eq!(load(&d).unwrap().as_int(), Some(*want), "{:?}", bytes);
        }
    }

    #[test]
    fn object_with_table() {
        // o :Foo, 2 ivars: @a = "hi", @t = u:Table "xx"
        let d: Vec<u8> = [&[4u8, 8, b'o', b':', 8][..], b"Foo", &[7, b':', 7], b"@a",
            &[b'"', 7], b"hi", &[b':', 7], b"@t", &[b'u', b':', 10], b"Table", &[7], b"xx"].concat();
        let v = load(&d).unwrap();
        assert_eq!(v.ivar("@a").and_then(|v| v.as_bytes()), Some(&b"hi"[..]));
        assert!(matches!(v.ivar("@t"), Some(Value::User { .. })));
    }
}
