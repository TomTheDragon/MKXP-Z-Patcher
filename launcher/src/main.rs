//! Tiny Windows launcher for patched games: it takes the place of Game.exe
//! (the original is kept as Game.original.exe), so shortcuts and Steam start mkxp-z.
//!
//! Starts mkxp-z\windows\mkxp-z.<cpu>.exe next to itself with the same arguments
//! (the engine that suits this computer's CPU best, see engine_order),
//! waits for it and returns its exit code. mkxp-z reads mkxp.json next to its
//! own .exe, and that config switches to the game folder. On Windows XP it
//! starts the legacy engine mkxp-z.xp.exe, the only one that runs there.
//!
//! Built without the standard library or a C runtime (no_std, raw-dylib
//! imports, linked by rust-lld), so it can be built on any OS without a Windows
//! SDK or MinGW and stays a few KB. 32-bit, so it runs on every Windows.

#![no_std]
#![no_main]
#![allow(non_snake_case, clippy::missing_safety_doc)]

use core::ffi::c_void;
use core::ptr::{null, null_mut};

type Handle = *mut c_void;

#[repr(C)]
struct FindData {
    attrs: u32,
    times: [u32; 6],
    size_hi: u32,
    size_lo: u32,
    reserved: [u32; 2],
    name: [u16; 260],
    alt_name: [u16; 14],
    // obsolete Mac fields follow in some SDKs; leave room
    _pad: [u32; 4],
}

#[repr(C)]
struct StartupInfo {
    cb: u32,
    reserved: *mut u16,
    desktop: *mut u16,
    title: *mut u16,
    x: u32,
    y: u32,
    xs: u32,
    ys: u32,
    xc: u32,
    yc: u32,
    fill: u32,
    flags: u32,
    show: u16,
    reserved2: u16,
    reserved3: *mut u8,
    stdin: Handle,
    stdout: Handle,
    stderr: Handle,
}

#[repr(C)]
struct ProcessInfo {
    process: Handle,
    thread: Handle,
    pid: u32,
    tid: u32,
}

#[link(name = "kernel32", kind = "raw-dylib", import_name_type = "undecorated")]
extern "system" {
    fn GetModuleFileNameW(module: Handle, buf: *mut u16, n: u32) -> u32;
    fn GetCommandLineW() -> *const u16;
    fn FindFirstFileW(pattern: *const u16, data: *mut FindData) -> Handle;
    fn FindClose(h: Handle) -> i32;
    fn CreateProcessW(app: *const u16, cmd: *mut u16, pa: *const c_void, ta: *const c_void, inherit: i32,
                      flags: u32, env: *const c_void, cwd: *const u16, si: *const StartupInfo,
                      pi: *mut ProcessInfo) -> i32;
    fn WaitForSingleObject(h: Handle, ms: u32) -> u32;
    fn GetExitCodeProcess(h: Handle, code: *mut u32) -> i32;
    fn CloseHandle(h: Handle) -> i32;
    fn GetLastError() -> u32;
    fn ExitProcess(code: u32) -> !;
    fn GetFileAttributesW(path: *const u16) -> u32;
    fn GetModuleHandleW(name: *const u16) -> Handle;
    fn GetProcAddress(module: Handle, name: *const u8) -> *const c_void;
    fn GetCurrentProcess() -> Handle;
    fn GetNativeSystemInfo(info: *mut SystemInfo);
}

#[repr(C)]
struct SystemInfo {
    arch: u16,
    reserved: u16,
    page_size: u32,
    min_addr: *mut c_void,
    max_addr: *mut c_void,
    active_mask: usize,
    processors: u32,
    processor_type: u32,
    granularity: u32,
    level: u16,
    revision: u16,
}

/// The computer's own CPU: IsWow64Process2 (Windows 10 1709+) tells it even
/// to emulated x86 code on ARM64; older Windows only run on x86/x64.
unsafe fn native_cpu() -> u16 {
    let k32: [u16; 9] = [b'k' as u16, b'e' as u16, b'r' as u16, b'n' as u16, b'e' as u16, b'l' as u16, b'3' as u16, b'2' as u16, 0];
    let f = GetProcAddress(GetModuleHandleW(k32.as_ptr()), b"IsWow64Process2\0".as_ptr());
    if !f.is_null() {
        let f: unsafe extern "system" fn(Handle, *mut u16, *mut u16) -> i32 = core::mem::transmute(f);
        let (mut process, mut native) = (0u16, 0u16);
        if f(GetCurrentProcess(), &mut process, &mut native) != 0 && native != 0 {
            return native;
        }
    }
    let mut si: SystemInfo = core::mem::zeroed();
    GetNativeSystemInfo(&mut si);
    match si.arch {
        12 => IMAGE_FILE_MACHINE_ARM64,
        9 => IMAGE_FILE_MACHINE_AMD64,
        _ => IMAGE_FILE_MACHINE_I386,
    }
}

const IMAGE_FILE_MACHINE_I386: u16 = 0x014c;
const IMAGE_FILE_MACHINE_AMD64: u16 = 0x8664;
const IMAGE_FILE_MACHINE_ARM64: u16 = 0xAA64;

/// Windows XP / Server 2003: no Vista functions (InitializeSRWLock came with Vista).
unsafe fn before_vista() -> bool {
    let k32: [u16; 9] = [b'k' as u16, b'e' as u16, b'r' as u16, b'n' as u16, b'e' as u16, b'l' as u16, b'3' as u16, b'2' as u16, 0];
    GetProcAddress(GetModuleHandleW(k32.as_ptr()), b"InitializeSRWLock\0".as_ptr()).is_null()
}

const XP_ENGINE: &str = "mkxp-z.xp.exe";

/// Engines to try, best first: the native one, then what this CPU runs
/// emulated (every Windows on ARM runs x86 code, only Windows 11 x64 code).
/// The legacy XP engine runs everywhere, but the others are better where
/// they run; it's found by the search for any engine as a last resort.
fn engine_order(cpu: u16) -> &'static [&'static str] {
    match cpu {
        IMAGE_FILE_MACHINE_ARM64 => &["mkxp-z.aarch64.exe", "mkxp-z.i686.exe", "mkxp-z.x86_64.exe"],
        IMAGE_FILE_MACHINE_AMD64 => &["mkxp-z.x86_64.exe", "mkxp-z.i686.exe"],
        _ => &["mkxp-z.i686.exe"],
    }
}

#[link(name = "user32", kind = "raw-dylib", import_name_type = "undecorated")]
extern "system" {
    fn MessageBoxW(hwnd: Handle, text: *const u16, caption: *const u16, kind: u32) -> i32;
}

#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    unsafe { ExitProcess(1) }
}

// No C runtime: the compiler may still emit calls to these.
#[no_mangle]
pub unsafe extern "C" fn memset(d: *mut c_void, c: i32, n: usize) -> *mut c_void {
    let d8 = d as *mut u8;
    for i in 0..n {
        core::ptr::write_volatile(d8.add(i), c as u8);
    }
    d
}
#[no_mangle]
pub unsafe extern "C" fn memcpy(d: *mut c_void, s: *const c_void, n: usize) -> *mut c_void {
    let (d8, s8) = (d as *mut u8, s as *const u8);
    for i in 0..n {
        core::ptr::write_volatile(d8.add(i), core::ptr::read_volatile(s8.add(i)));
    }
    d
}
#[no_mangle]
pub unsafe extern "C" fn memmove(d: *mut c_void, s: *const c_void, n: usize) -> *mut c_void {
    if (d as usize) < (s as usize) {
        memcpy(d, s, n)
    } else {
        let (d8, s8) = (d as *mut u8, s as *const u8);
        let mut i = n;
        while i > 0 {
            i -= 1;
            core::ptr::write_volatile(d8.add(i), core::ptr::read_volatile(s8.add(i)));
        }
        d
    }
}
#[no_mangle]
pub unsafe extern "C" fn memcmp(a: *const c_void, b: *const c_void, n: usize) -> i32 {
    let (a8, b8) = (a as *const u8, b as *const u8);
    for i in 0..n {
        let (x, y) = (core::ptr::read_volatile(a8.add(i)), core::ptr::read_volatile(b8.add(i)));
        if x != y {
            return x as i32 - y as i32;
        }
    }
    0
}

const N: usize = 32768;

/// A fixed UTF-16 string buffer in static memory (keeps the stack small: no __chkstk).
struct Buf {
    data: *mut u16,
    len: usize,
}

impl Buf {
    fn push(&mut self, c: u16) {
        if self.len + 1 < N {
            unsafe { *self.data.add(self.len) = c };
            self.len += 1;
        }
    }
    fn push_str(&mut self, s: &str) {
        for c in s.encode_utf16() {
            self.push(c);
        }
    }
    unsafe fn push_wide(&mut self, mut p: *const u16) {
        while *p != 0 {
            self.push(*p);
            p = p.add(1);
        }
    }
    fn push_num(&mut self, mut n: u32) {
        let mut digits = [0u16; 10];
        let mut i = 0;
        loop {
            digits[i] = b'0' as u16 + (n % 10) as u16;
            n /= 10;
            i += 1;
            if n == 0 {
                break;
            }
        }
        while i > 0 {
            i -= 1;
            self.push(digits[i]);
        }
    }
    fn terminated(&mut self) -> *mut u16 {
        unsafe { *self.data.add(self.len) = 0 };
        self.data
    }
}

static mut EXE: [u16; N] = [0; N];
static mut DIR: [u16; N] = [0; N];
static mut TARGET: [u16; N] = [0; N];
static mut CMD: [u16; N] = [0; N];
static mut MSG: [u16; N] = [0; N];

unsafe fn buf(p: *mut [u16; N]) -> Buf {
    Buf { data: p as *mut u16, len: 0 }
}

unsafe fn fail(what: &str, path: &Buf, code: Option<u32>) -> ! {
    let mut m = buf(core::ptr::addr_of_mut!(MSG));
    m.push_str(what);
    m.push_str("\n\n");
    for i in 0..path.len {
        m.push(*path.data.add(i));
    }
    if let Some(c) = code {
        m.push_str("\n\nWindows error ");
        m.push_num(c);
    }
    let title: [u16; 7] = [b'm' as u16, b'k' as u16, b'x' as u16, b'p' as u16, b'-' as u16, b'z' as u16, 0];
    MessageBoxW(null_mut(), m.terminated(), title.as_ptr(), 0x10 /* MB_ICONERROR */);
    ExitProcess(1)
}

#[no_mangle]
pub unsafe extern "system" fn entry() -> ! {
    // folder of this launcher = the game folder
    let n = GetModuleFileNameW(null_mut(), core::ptr::addr_of_mut!(EXE) as *mut u16, N as u32) as usize;
    let exe = &*core::ptr::addr_of!(EXE);
    let mut cut = 0;
    for i in 0..n.min(N) {
        if exe[i] == b'\\' as u16 || exe[i] == b'/' as u16 {
            cut = i;
        }
    }
    let mut dir = buf(core::ptr::addr_of_mut!(DIR));
    for &c in &exe[..cut] {
        dir.push(c);
    }

    // find mkxp-z\windows\mkxp-z.<cpu>.exe: the best for this CPU, else any
    let mut target = buf(core::ptr::addr_of_mut!(TARGET));
    for i in 0..dir.len {
        target.push(*dir.data.add(i));
    }
    target.push_str("\\mkxp-z\\windows\\");
    let base_len = target.len;
    if before_vista() {
        // only the legacy engine starts on XP (the others need newer Windows)
        target.push_str(XP_ENGINE);
        if GetFileAttributesW(target.terminated()) == 0xFFFF_FFFF {
            fail("This game was patched without the Windows XP engine, and the other mkxp-z engines need \
                  Windows 7 or newer. Patch it again with \"Windows legacy (XP)\" chosen. Expected:", &target, None);
        }
    }
    let mut found = before_vista();
    for name in engine_order(native_cpu()) {
        if found {
            break; // XP: its engine, found above
        }
        target.len = base_len;
        target.push_str(name);
        if GetFileAttributesW(target.terminated()) != 0xFFFF_FFFF {
            found = true;
            break;
        }
    }
    if !found {
        target.len = base_len;
        target.push_str("mkxp-z.*.exe");
        let mut fd: FindData = core::mem::zeroed();
        let h = FindFirstFileW(target.terminated(), &mut fd);
        if h as isize == -1 {
            target.len = base_len;
            fail("The mkxp-z engine for this game wasn't found. Expected it in:", &target, None);
        }
        FindClose(h);
        target.len = base_len;
        target.push_wide(fd.name.as_ptr());
    }

    // command line: "<engine>" + our own arguments
    let mut cmd = buf(core::ptr::addr_of_mut!(CMD));
    cmd.push(b'"' as u16);
    for i in 0..target.len {
        cmd.push(*target.data.add(i));
    }
    cmd.push(b'"' as u16);
    let mut p = GetCommandLineW();
    if *p == b'"' as u16 {
        p = p.add(1);
        while *p != 0 && *p != b'"' as u16 {
            p = p.add(1);
        }
        if *p != 0 {
            p = p.add(1);
        }
    } else {
        while *p != 0 && *p != b' ' as u16 && *p != b'\t' as u16 {
            p = p.add(1);
        }
    }
    cmd.push_wide(p); // rest starts with the separating space, if any

    let mut si: StartupInfo = core::mem::zeroed();
    si.cb = core::mem::size_of::<StartupInfo>() as u32;
    let mut pi: ProcessInfo = core::mem::zeroed();
    let ok = CreateProcessW(target.terminated(), cmd.terminated(), null(), null(), 0, 0, null(),
                            dir.terminated(), &si, &mut pi);
    if ok == 0 {
        fail("Couldn't start the game engine:", &target, Some(GetLastError()));
    }
    CloseHandle(pi.thread);
    WaitForSingleObject(pi.process, 0xFFFF_FFFF);
    let mut code = 0u32;
    GetExitCodeProcess(pi.process, &mut code);
    CloseHandle(pi.process);
    ExitProcess(code)
}
