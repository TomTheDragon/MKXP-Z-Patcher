//! The window on Windows: native Win32 controls only (user32/gdi32/shell32,
//! part of every Windows), so the patcher is a single .exe without GTK or
//! other DLLs. Same behaviour as the GTK window on Linux: slow work runs on
//! worker threads that report back through a channel polled with a timer.

use crate::engines::{self, Choice, Cpu, Paths, Platform, System};
use crate::patch;
use crate::plan::{Options, Plan};
use crate::util::clean_path;
use std::cell::RefCell;
use std::path::{Path, PathBuf};
use std::sync::mpsc::{channel, Receiver, Sender};
use windows_sys::Win32::Foundation::{HWND, LPARAM, LRESULT, RECT, WPARAM};
use windows_sys::Win32::Graphics::Gdi::{BitBlt, CreateCompatibleBitmap, CreateCompatibleDC, CreateFontIndirectW, DeleteDC,
                                        DeleteObject, GetDC, GetDIBits, GetDeviceCaps, GetSysColor, GetSysColorBrush,
                                        ReleaseDC, SelectObject, SetBkColor, UpdateWindow, BITMAPINFO, BITMAPINFOHEADER,
                                        COLOR_WINDOW, HDC, LOGFONTW, SRCCOPY};
use windows_sys::Win32::System::Com::{CoInitializeEx, CoTaskMemFree};
use windows_sys::Win32::System::LibraryLoader::{GetModuleHandleW, LoadLibraryW};
use windows_sys::Win32::UI::Controls::{InitCommonControlsEx, INITCOMMONCONTROLSEX};
use windows_sys::Win32::UI::Input::KeyboardAndMouse::EnableWindow;
use windows_sys::Win32::UI::Shell::{DragAcceptFiles, DragFinish, DragQueryFileW, HDROP};

// The folder picker (shell32, part of every Windows). Declared here because
// windows-sys 0.52 hides it behind a feature it doesn't actually offer.
#[repr(C)]
#[allow(non_snake_case)]
struct BROWSEINFOW {
    hwndOwner: HWND,
    pidlRoot: *mut std::ffi::c_void,
    pszDisplayName: *mut u16,
    lpszTitle: *const u16,
    ulFlags: u32,
    lpfn: Option<unsafe extern "system" fn(HWND, u32, LPARAM, LPARAM) -> i32>,
    lParam: LPARAM,
    iImage: i32,
}

#[link(name = "shell32")]
extern "system" {
    fn SHBrowseForFolderW(bi: *const BROWSEINFOW) -> *mut std::ffi::c_void;
    fn SHGetPathFromIDListW(pidl: *const std::ffi::c_void, path: *mut u16) -> i32;
}
use windows_sys::Win32::UI::WindowsAndMessaging::*;

// ------------------------------------------------------------------ constants

const ID_EDIT: [i32; 4] = [101, 102, 103, 104]; // game, engines, rtp, source
const ID_BROWSE: [i32; 4] = [201, 202, 203, 204];
// radio buttons: the systems (System::ALL), then the CPU types (Cpu::ALL)
const ID_CHOICE: [i32; 10] = [301, 302, 303, 304, 305, 312, 306, 307, 308, 311];
const ID_SOUNDFONT: i32 = 309;
const ID_REPLACE_EXE: i32 = 310;
const ID_PATCH: i32 = 501;
const ID_UNPATCH: i32 = 502;
const ID_START: i32 = 503;
const TIMER_POLL: usize = 1;
const TIMER_ANALYZE: usize = 2;
const TIMER_SCREENSHOT: usize = 3;

// RichEdit (Msftedit.dll, part of every Windows) for the coloured info text
const EM_SETCHARFORMAT: u32 = 0x0400 + 68;
const EM_SETBKGNDCOLOR: u32 = 0x0400 + 67;
const SCF_SELECTION: usize = 0x0001;
const CFM_COLOR: u32 = 0x4000_0000;
const CFE_AUTOCOLOR: u32 = 0x4000_0000;
const COLOR_WARN: u32 = 0x0000_70D0; // COLORREF = 0x00BBGGRR: orange
const COLOR_ERR: u32 = 0x0030_30D0; // red
const COLOR_OK: u32 = 0x0044_9E2E; // green

#[repr(C)]
#[allow(non_snake_case)]
struct CHARFORMATW {
    cbSize: u32,
    dwMask: u32,
    dwEffects: u32,
    yHeight: i32,
    yOffset: i32,
    crTextColor: u32,
    bCharSet: u8,
    bPitchAndFamily: u8,
    szFaceName: [u16; 32],
}

const EN_CHANGE: u32 = 0x0300;
const BN_CLICKED: u32 = 0;
const BM_GETCHECK: u32 = 0x00F0;
const BM_SETCHECK: u32 = 0x00F1;
const EM_SETSEL: u32 = 0x00B1;
const EM_REPLACESEL: u32 = 0x00C2;
const EM_SETLIMITTEXT: u32 = 0x00C5;
const EM_SETCUEBANNER: u32 = 0x1501;
const WM_SETFONT_: u32 = 0x0030;

const ES_AUTOHSCROLL: u32 = 0x0080;
const ES_MULTILINE: u32 = 0x0004;
const ES_READONLY: u32 = 0x0800;
const ES_AUTOVSCROLL: u32 = 0x0040;
const BS_PUSHBUTTON: u32 = 0;
const BS_DEFPUSHBUTTON: u32 = 1;
const BS_AUTOCHECKBOX: u32 = 3;
const BS_AUTORADIOBUTTON: u32 = 9;
const WS_GROUP_: u32 = 0x0002_0000;
const BS_GROUPBOX: u32 = 7;
const SS_LEFT: u32 = 0;
const SS_CENTERIMAGE: u32 = 0x0200;

const BIF_RETURNONLYFSDIRS: u32 = 0x0001;
const BIF_EDITBOX: u32 = 0x0010;
const BIF_NEWDIALOGSTYLE: u32 = 0x0040;
const BFFM_INITIALIZED: u32 = 1;
const BFFM_SETSELECTIONW: u32 = 0x0400 + 103;

fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(Some(0)).collect()
}

// ------------------------------------------------------------------ state

enum Msg {
    Analyzed(u64, Result<Plan, String>),
    Log(String),
    Finished(Result<String, String>),
}

#[derive(PartialEq, Clone)]
struct Key {
    game: String,
    engines: String,
    source: String,
    rtp: String,
    choice: Choice,
    platforms: Vec<Platform>,
    soundfont: bool,
    replace_exe: bool,
}

#[derive(Default)]
struct Controls {
    labels: Vec<HWND>,
    edits: [HWND; 4],
    browse: [HWND; 4],
    group: HWND,
    /// "System:" and "CPU:"
    headings: [HWND; 2],
    /// see ID_CHOICE
    choice: [HWND; 10],
    soundfont: HWND,
    replace_exe: HWND,
    hint: HWND,
    summary: HWND,
    log: HWND,
    status: HWND,
    rich: bool,
    patch: HWND,
    unpatch: HWND,
    start: HWND,
    scale: f32,
}

struct App {
    c: Controls,
    plan: Option<Plan>,
    plan_error: Option<String>,
    analyzed: Option<Key>,
    pending: Option<(u64, Key)>,
    generation: u64,
    busy: Option<String>,
    result: Option<Result<String, String>>,
    linux_engine: Option<PathBuf>,
    msys2: PathBuf,
    tx: Sender<Msg>,
    rx: Receiver<Msg>,
}

thread_local! {
    static APP: RefCell<Option<App>> = const { RefCell::new(None) };
}

fn with<R>(f: impl FnOnce(&mut App) -> R) -> Option<R> {
    APP.with(|a| a.try_borrow_mut().ok().and_then(|mut a| a.as_mut().map(f)))
}

// ------------------------------------------------------------------ helpers

fn get_text(h: HWND) -> String {
    unsafe {
        let n = GetWindowTextLengthW(h);
        let mut buf = vec![0u16; n as usize + 1];
        let got = GetWindowTextW(h, buf.as_mut_ptr(), buf.len() as i32);
        String::from_utf16_lossy(&buf[..got.max(0) as usize])
    }
}

fn set_text(h: HWND, s: &str) {
    unsafe { SetWindowTextW(h, wide(s).as_ptr()) };
}

/// Edit controls want Windows line breaks.
fn crlf(s: &str) -> String {
    s.replace("\r\n", "\n").replace('\n', "\r\n")
}

fn checked(h: HWND) -> bool {
    unsafe { SendMessageW(h, BM_GETCHECK, 0, 0) == 1 }
}

fn set_checked(h: HWND, on: bool) {
    unsafe { SendMessageW(h, BM_SETCHECK, on as usize, 0) };
}

fn enable(h: HWND, on: bool) {
    unsafe { EnableWindow(h, on as i32) };
}

fn message(owner: HWND, text: &str, flags: u32) -> i32 {
    unsafe { MessageBoxW(owner, wide(text).as_ptr(), wide("MKXP-Z Patcher").as_ptr(), flags) }
}

fn ask(owner: HWND, title: &str, text: &str) -> bool {
    message(owner, &format!("{}\n\n{}", title, text), MB_YESNO | MB_ICONQUESTION) == IDYES
}

unsafe extern "system" fn browse_init(hwnd: HWND, msg: u32, _: LPARAM, data: LPARAM) -> i32 {
    if msg == BFFM_INITIALIZED && data != 0 {
        SendMessageW(hwnd, BFFM_SETSELECTIONW, 1, data);
    }
    0
}

/// The system folder picker; returns the chosen folder.
fn pick_folder(owner: HWND, title: &str, current: &str) -> Option<String> {
    let title_w = wide(title);
    let start_w = wide(current);
    let mut name = vec![0u16; 260];
    let bi = BROWSEINFOW {
        hwndOwner: owner,
        pidlRoot: std::ptr::null_mut(),
        pszDisplayName: name.as_mut_ptr(),
        lpszTitle: title_w.as_ptr(),
        ulFlags: BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX,
        lpfn: Some(browse_init),
        lParam: if Path::new(current).is_dir() { start_w.as_ptr() as LPARAM } else { 0 },
        iImage: 0,
    };
    unsafe {
        let pidl = SHBrowseForFolderW(&bi);
        if pidl.is_null() {
            return None;
        }
        let mut path = vec![0u16; 32768];
        let ok = SHGetPathFromIDListW(pidl, path.as_mut_ptr());
        CoTaskMemFree(pidl as *const _);
        if ok == 0 {
            return None;
        }
        let len = path.iter().position(|c| *c == 0).unwrap_or(0);
        Some(String::from_utf16_lossy(&path[..len]))
    }
}

// ------------------------------------------------------------------ window

pub fn run(game: Option<PathBuf>, paths: Paths, linux_engine: Option<PathBuf>) -> Result<(), String> {
    unsafe {
        SetProcessDPIAware();
        CoInitializeEx(std::ptr::null(), 0x2 /* COINIT_APARTMENTTHREADED */);
        let icc = INITCOMMONCONTROLSEX { dwSize: std::mem::size_of::<INITCOMMONCONTROLSEX>() as u32, dwICC: 0x0000_4000 | 0x0000_00FF };
        InitCommonControlsEx(&icc);
    }
    let hinst = unsafe { GetModuleHandleW(std::ptr::null()) };
    let class = wide("RpgmPatcherWindow");
    let wc = WNDCLASSEXW {
        cbSize: std::mem::size_of::<WNDCLASSEXW>() as u32,
        style: 0,
        lpfnWndProc: Some(wndproc),
        cbClsExtra: 0,
        cbWndExtra: 0,
        hInstance: hinst,
        hIcon: unsafe { LoadIconW(hinst, 1 as _) },
        hCursor: unsafe { LoadCursorW(0, IDC_ARROW) },
        hbrBackground: unsafe { GetSysColorBrush(COLOR_WINDOW) },
        lpszMenuName: std::ptr::null(),
        lpszClassName: class.as_ptr(),
        hIconSm: unsafe { LoadIconW(hinst, 1 as _) },
    };
    if unsafe { RegisterClassExW(&wc) } == 0 {
        return Err("couldn't register the window class".into());
    }
    let scale = unsafe {
        let dc: HDC = GetDC(0);
        let dpi = GetDeviceCaps(dc, 90 /* LOGPIXELSY */);
        ReleaseDC(0, dc);
        dpi.max(96) as f32 / 96.0
    };
    let px = |v: f32| (v * scale) as i32;
    let hwnd = unsafe {
        CreateWindowExW(0, class.as_ptr(), wide("MKXP-Z Patcher").as_ptr(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                        CW_USEDEFAULT, CW_USEDEFAULT, px(1290.0), px(720.0), 0, 0, hinst, std::ptr::null())
    };
    if hwnd == 0 {
        return Err("couldn't create the window".into());
    }
    let c = create_controls(hwnd, hinst, scale);
    let paths = paths.resolved();
    set_text(c.edits[0], &game.map(|g| g.display().to_string()).unwrap_or_default());
    set_text(c.edits[1], &paths.engines.display().to_string());
    set_text(c.edits[3], &paths.source.display().to_string());
    let host = Choice::for_host();
    set_choice(&c, host);
    set_checked(c.soundfont, true);
    set_checked(c.replace_exe, true);
    let (tx, rx) = channel();
    APP.with(|a| *a.borrow_mut() = Some(App {
        c, plan: None, plan_error: None, analyzed: None, pending: None, generation: 0, busy: None, result: None,
        linux_engine, msys2: paths.msys2.clone(), tx, rx,
    }));
    unsafe {
        DragAcceptFiles(hwnd, 1);
        SetTimer(hwnd, TIMER_POLL, 100, None);
        layout(hwnd);
        ShowWindow(hwnd, SW_SHOWDEFAULT);
        UpdateWindow(hwnd);
    }
    with(|a| {
        update_controls(a);
        render_summary(a);
    });
    schedule_analyze(hwnd, 0);
    if std::env::var_os("MKXPZ_SCREENSHOT").is_some() {
        let ms = std::env::var("MKXPZ_SCREENSHOT_DELAY").ok().and_then(|v| v.parse().ok()).unwrap_or(3000);
        unsafe { SetTimer(hwnd, TIMER_SCREENSHOT, ms, None) };
    }
    let mut msg: MSG = unsafe { std::mem::zeroed() };
    unsafe {
        while GetMessageW(&mut msg, 0, 0, 0) > 0 {
            if IsDialogMessageW(hwnd, &msg) == 0 {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
    Ok(())
}

fn create_controls(parent: HWND, hinst: isize, scale: f32) -> Controls {
    let font = unsafe {
        let mut ncm: NONCLIENTMETRICSW = std::mem::zeroed();
        ncm.cbSize = std::mem::size_of::<NONCLIENTMETRICSW>() as u32;
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, ncm.cbSize, &mut ncm as *mut _ as *mut _, 0);
        CreateFontIndirectW(&ncm.lfMessageFont)
    };
    let mono = unsafe {
        let mut lf: LOGFONTW = std::mem::zeroed();
        lf.lfHeight = -((9.0 * scale * 96.0 / 72.0) as i32);
        for (i, c) in "Consolas".encode_utf16().enumerate() {
            lf.lfFaceName[i] = c;
        }
        CreateFontIndirectW(&lf)
    };
    let make = |class: &str, text: &str, style: u32, ex: u32, id: i32, f: isize| -> HWND {
        let h = unsafe {
            CreateWindowExW(ex, wide(class).as_ptr(), wide(text).as_ptr(), WS_CHILD | WS_VISIBLE | style,
                            0, 0, 10, 10, parent, id as isize, hinst, std::ptr::null())
        };
        unsafe { SendMessageW(h, WM_SETFONT_, f as usize, 1) };
        h
    };
    let mut c = Controls { scale, ..Default::default() };
    let rows = [("Game folder", "the folder with Game.exe / Game.ini - type, paste, browse or drop it here"),
                ("Engines folder", "built engines (created as needed)"),
                ("RTP (optional)", "only for games that need the RPG Maker RTP"),
                ("mkxp-z source", "mkxp-z source (fetched when an engine must be built)")];
    for (i, (label, hint)) in rows.iter().enumerate() {
        c.labels.push(make("STATIC", label, SS_LEFT | SS_CENTERIMAGE, 0, 0, font));
        c.edits[i] = make("EDIT", "", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, ID_EDIT[i], font);
        unsafe { SendMessageW(c.edits[i], EM_SETCUEBANNER, 1, wide(hint).as_ptr() as LPARAM) };
        c.browse[i] = make("BUTTON", "Browse…", WS_TABSTOP | BS_PUSHBUTTON, 0, ID_BROWSE[i], font);
    }
    c.group = make("BUTTON", "Patch for", BS_GROUPBOX, 0, 0, font);
    c.headings = ["System:", "CPU:"].map(|t| make("STATIC", t, SS_LEFT | SS_CENTERIMAGE, 0, 0, font));
    // Windows legacy: only the 32-bit engine for Windows XP (Game.exe also starts it on newer Windows)
    // Android: APKs are made on Linux only (Google's NDK), so it's shown but off here
    // Linux legacy: built in an Ubuntu 20.04 container (on Linux)
    // Nintendo 3DS: the game converted for MKXP-Z 3DS, in a folder next to it (n3ds.rs)
    let names = ["Linux", "Linux legacy", "Windows", "Windows legacy (XP)", "Android (on Linux)", "Nintendo 3DS", "32-bit",
                 "64-bit", "ARM 32-bit", "ARM 64-bit"];
    let first_cpu = System::ALL.len();
    for i in 0..names.len() {
        // WS_GROUP starts each set of radio buttons (systems, CPU types)
        let group = if i == 0 || i == first_cpu { WS_GROUP_ | WS_TABSTOP } else { 0 };
        c.choice[i] = make("BUTTON", names[i], group | BS_AUTORADIOBUTTON, 0, ID_CHOICE[i], font);
    }
    c.soundfont = make("BUTTON", "bundle a SoundFont for MIDI music", WS_GROUP_ | WS_TABSTOP | BS_AUTOCHECKBOX, 0,
                       ID_SOUNDFONT, font);
    // off: Game.mkxp-z.exe / Game.mkxp-z.64.exe next to Game.exe (Steam versions then need launch options)
    c.replace_exe = make("BUTTON", "replace Game.exe (the original is kept as Game.original.exe)",
                         WS_TABSTOP | BS_AUTOCHECKBOX, 0, ID_REPLACE_EXE, font);
    c.hint = make("STATIC", "32-bit Windows runs the game's own 32-bit DLLs natively (e.g. Steam's); 64-bit and ARM \
                            can't load them. Windows legacy: XP and newer (32-bit only).",
                  SS_LEFT, 0, 0, font);
    let text_style = WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL;
    let rich = unsafe { LoadLibraryW(wide("Msftedit.dll").as_ptr()) } != 0;
    c.summary = make(if rich { "RICHEDIT50W" } else { "EDIT" }, "", text_style, WS_EX_CLIENTEDGE, 0, font);
    c.rich = rich;
    if rich {
        unsafe { SendMessageW(c.summary, EM_SETBKGNDCOLOR, 0, GetSysColor(COLOR_WINDOW) as LPARAM) };
    }
    c.log = make("EDIT", "", text_style, WS_EX_CLIENTEDGE, 0, mono);
    for h in [c.summary, c.log] {
        unsafe { SendMessageW(h, EM_SETLIMITTEXT, 0x7FFF_FFFE, 0) };
    }
    c.status = make("STATIC", "", SS_LEFT | SS_CENTERIMAGE, 0, 0, font);
    c.start = make("BUTTON", "Start game", WS_TABSTOP | BS_PUSHBUTTON, 0, ID_START, font);
    c.unpatch = make("BUTTON", "Remove patch", WS_TABSTOP | BS_PUSHBUTTON, 0, ID_UNPATCH, font);
    c.patch = make("BUTTON", "Patch game", WS_TABSTOP | BS_DEFPUSHBUTTON, 0, ID_PATCH, font);
    c
}

fn layout(hwnd: HWND) {
    with(|a| {
        let c = &a.c;
        let mut r: RECT = unsafe { std::mem::zeroed() };
        unsafe { GetClientRect(hwnd, &mut r) };
        let px = |v: f32| (v * c.scale) as i32;
        let (m, gap, rowh, lw, bw) = (px(12.0), px(6.0), px(24.0), px(110.0), px(90.0));
        let width = r.right - r.left;
        let height = r.bottom - r.top;
        let mv = |h: HWND, x: i32, y: i32, w: i32, hh: i32| unsafe { MoveWindow(h, x, y, w.max(1), hh.max(1), 1) };
        let mut y = m;
        for i in 0..4 {
            mv(c.labels[i], m, y, lw, rowh);
            mv(c.edits[i], m + lw + gap, y, width - 2 * m - lw - bw - 2 * gap, rowh);
            mv(c.browse[i], width - m - bw, y, bw, rowh);
            y += rowh + gap;
        }
        // platform group
        let gh = rowh * 3 + px(38.0);
        mv(c.group, m, y, width - 2 * m, gh);
        let (gx, gy) = (m + px(12.0), y + px(20.0));
        let widths = [70.0, 115.0, 90.0, 160.0, 150.0, 115.0, 75.0, 75.0, 100.0, 100.0];
        let first_cpu = System::ALL.len();
        let mut x = gx;
        for i in 0..widths.len() {
            if i == 0 || i == first_cpu {
                let hw = px(if i == 0 { 60.0 } else { 40.0 });
                if i == first_cpu {
                    x += px(20.0);
                }
                mv(c.headings[if i == 0 { 0 } else { 1 }], x, gy, hw, rowh);
                x += hw;
            }
            mv(c.choice[i], x, gy, px(widths[i]), rowh);
            x += px(widths[i]) + px(6.0);
        }
        mv(c.soundfont, gx, gy + rowh + px(4.0), px(260.0), rowh);
        mv(c.replace_exe, gx + px(270.0), gy + rowh + px(4.0), px(440.0), rowh);
        mv(c.hint, gx, gy + 2 * (rowh + px(4.0)), width - 2 * m - px(24.0), rowh);
        y += gh + gap;
        // text areas and buttons
        let bottom_h = px(30.0);
        let text_h = (height - y - m - bottom_h - gap).max(px(80.0));
        let summary_h = text_h * 45 / 100;
        mv(c.summary, m, y, width - 2 * m, summary_h);
        mv(c.log, m, y + summary_h + gap, width - 2 * m, text_h - summary_h - gap);
        let by = height - m - bottom_h;
        let bw2 = px(130.0);
        mv(c.patch, width - m - bw2, by, bw2, bottom_h);
        mv(c.unpatch, width - m - 2 * bw2 - gap, by, bw2, bottom_h);
        mv(c.start, width - m - 3 * bw2 - 2 * gap, by, bw2, bottom_h);
        mv(c.status, m, by, width - 2 * m - 3 * bw2 - 3 * gap, bottom_h);
    });
}

unsafe extern "system" fn wndproc(hwnd: HWND, msg: u32, wp: WPARAM, lp: LPARAM) -> LRESULT {
    match msg {
        WM_SIZE => {
            layout(hwnd);
            0
        }
        WM_GETMINMAXINFO => {
            let mmi = lp as *mut MINMAXINFO;
            let s = with(|a| a.c.scale).unwrap_or(1.0);
            (*mmi).ptMinTrackSize.x = (720.0 * s) as i32;
            (*mmi).ptMinTrackSize.y = (520.0 * s) as i32;
            0
        }
        WM_CTLCOLORSTATIC => {
            // labels, checkboxes, the group box and the read-only text areas all on
            // the window background, so nothing shows up as a differently grey box
            SetBkColor(wp as HDC, GetSysColor(COLOR_WINDOW));
            GetSysColorBrush(COLOR_WINDOW) as LRESULT
        }
        WM_COMMAND => {
            let id = (wp & 0xFFFF) as i32;
            let code = ((wp >> 16) & 0xFFFF) as u32;
            on_command(hwnd, id, code);
            0
        }
        WM_TIMER => {
            match wp {
                TIMER_POLL => poll(),
                TIMER_ANALYZE => {
                    KillTimer(hwnd, TIMER_ANALYZE);
                    analyze(false);
                }
                TIMER_SCREENSHOT => {
                    KillTimer(hwnd, TIMER_SCREENSHOT);
                    if let Some(p) = std::env::var_os("MKXPZ_SCREENSHOT") {
                        screenshot(hwnd, Path::new(&p));
                    }
                    DestroyWindow(hwnd);
                }
                _ => {}
            }
            0
        }
        WM_DROPFILES => {
            let drop = wp as HDROP;
            let mut buf = vec![0u16; 32768];
            let n = DragQueryFileW(drop, 0, buf.as_mut_ptr(), buf.len() as u32) as usize;
            DragFinish(drop);
            if n > 0 && with(|a| a.busy.is_none()).unwrap_or(false) {
                let p = PathBuf::from(String::from_utf16_lossy(&buf[..n]));
                let dir = if p.is_dir() { p } else { p.parent().map(Path::to_path_buf).unwrap_or(p) };
                if let Some(h) = with(|a| a.c.edits[0]) {
                    set_text(h, &dir.display().to_string());
                }
            }
            0
        }
        WM_DESTROY => {
            PostQuitMessage(0);
            0
        }
        _ => DefWindowProcW(hwnd, msg, wp, lp),
    }
}

fn on_command(hwnd: HWND, id: i32, code: u32) {
    if let Some(i) = ID_EDIT.iter().position(|x| *x == id) {
        if code == EN_CHANGE {
            if let Some(h) = with(|a| a.c.edits[i]) {
                let t = get_text(h);
                if t.starts_with("file://") || t.starts_with('"') || t.starts_with('\'') {
                    set_text(h, &clean_path(&t)); // sends EN_CHANGE again with the clean text
                    return;
                }
            }
            schedule_analyze(hwnd, 300);
        }
        return;
    }
    if let Some(i) = ID_BROWSE.iter().position(|x| *x == id) {
        let titles = ["Select the RPG Maker game folder", "Select the engines folder", "Select the RTP folder",
                      "Select the mkxp-z source folder"];
        if let Some(h) = with(|a| a.c.edits[i]) {
            if let Some(p) = pick_folder(hwnd, titles[i], &get_text(h)) {
                set_text(h, &p);
            }
        }
        return;
    }
    match (id, code) {
        (id, BN_CLICKED) if ID_CHOICE.contains(&id) || id == ID_SOUNDFONT || id == ID_REPLACE_EXE => {
            with(|a| {
                // a CPU type the new system has no engine for: 32-bit instead
                let c = choice(&a.c);
                if c.fixed() != c {
                    set_choice(&a.c, c.fixed());
                }
            });
            with(update_controls);
            schedule_analyze(hwnd, 0);
        }
        (ID_PATCH, BN_CLICKED) => start_patch(hwnd),
        (ID_UNPATCH, BN_CLICKED) => {
            let game = with(|a| get_text(a.c.edits[0])).unwrap_or_default();
            if ask(hwnd, "Remove the patch?", &format!("Remove the mkxp-z files from\n{}?\n\nGame files and save games \
                                                         are not touched.", game.trim())) {
                do_unpatch();
            }
        }
        (ID_START, BN_CLICKED) => start_game(),
        _ => {}
    }
}

// ------------------------------------------------------------------ logic

fn choice(c: &Controls) -> Choice {
    let (sys, cpu) = c.choice.split_at(System::ALL.len());
    let pick = |hs: &[HWND]| hs.iter().position(|h| checked(*h)).unwrap_or(0);
    Choice { system: System::ALL[pick(sys)], cpu: Cpu::ALL[pick(cpu)] }
}

fn set_choice(c: &Controls, ch: Choice) {
    let (sys, cpu) = c.choice.split_at(System::ALL.len());
    for (h, s) in sys.iter().zip(System::ALL) {
        set_checked(*h, s == ch.system);
    }
    for (h, x) in cpu.iter().zip(Cpu::ALL) {
        set_checked(*h, x == ch.cpu);
    }
}

fn key(c: &Controls) -> Key {
    let choice = choice(c);
    let platforms = choice.platforms();
    Key { game: get_text(c.edits[0]).trim().into(), engines: get_text(c.edits[1]).trim().into(),
          rtp: get_text(c.edits[2]).trim().into(), source: get_text(c.edits[3]).trim().into(),
          choice, platforms, soundfont: checked(c.soundfont), replace_exe: checked(c.replace_exe) }
}

fn options(a: &App, k: &Key) -> Options {
    Options {
        platforms: k.platforms.clone(),
        paths: Paths { engines: PathBuf::from(&k.engines), source: PathBuf::from(&k.source), msys2: a.msys2.clone() },
        linux_engine: a.linux_engine.clone(),
        rtp: (!k.rtp.is_empty()).then(|| PathBuf::from(&k.rtp)),
        rgss: None,
        bundle_soundfont: k.soundfont,
        replace_exe: k.replace_exe,
        n3ds: k.choice.system == System::N3ds,
    }
}

fn schedule_analyze(hwnd: HWND, delay_ms: u32) {
    unsafe { SetTimer(hwnd, TIMER_ANALYZE, delay_ms.max(1), None) };
}

fn analyze(force: bool) {
    with(|a| {
        if a.busy.is_some() {
            return;
        }
        let k = key(&a.c);
        if !force && (a.analyzed.as_ref() == Some(&k) || a.pending.as_ref().map(|(_, p)| p) == Some(&k)) {
            return;
        }
        if k.game.is_empty() || !Path::new(&k.game).is_dir() || k.platforms.is_empty() && k.choice.system != System::N3ds {
            a.plan = None;
            a.plan_error = None;
            a.analyzed = Some(k);
            render_summary(a);
            update_controls(a);
            return;
        }
        a.generation += 1;
        let gen = a.generation;
        a.pending = Some((gen, k.clone()));
        let opts = options(a, &k);
        let tx = a.tx.clone();
        std::thread::spawn(move || {
            let _ = tx.send(Msg::Analyzed(gen, Plan::new(Path::new(&k.game), &opts)));
        });
        update_controls(a);
    });
}

fn spawn_task(label: &str, f: impl FnOnce(&(dyn Fn(String) + Sync)) -> Result<String, String> + Send + 'static) {
    with(|a| {
        a.busy = Some(label.into());
        a.result = None;
        set_text(a.c.log, "");
        update_controls(a);
        render_summary(a);
        let tx = a.tx.clone();
        std::thread::spawn(move || {
            let log_tx = std::sync::Mutex::new(tx.clone());
            let log = move |s: String| {
                let _ = log_tx.lock().map(|t| t.send(Msg::Log(s)));
            };
            let r = f(&log);
            let _ = tx.send(Msg::Finished(r));
        });
    });
}

/// the Nintendo 3DS version: made next to the game folder (n3ds.rs)
fn start_n3ds(hwnd: HWND) {
    let Some((k, warnings)) = with(|a| (key(&a.c), a.plan.as_ref().map(|p| p.warnings.clone()).unwrap_or_default()))
        else { return };
    let game = PathBuf::from(&k.game);
    let name = game.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    let mut text = format!("This makes \"{} (3DS)\" next to the game folder (which stays as it is): the app MKXP-Z 3DS \
                            and the game, its images and sound converted for the 3DS. That takes a minute or two.", name);
    if !warnings.is_empty() {
        text.push_str(&format!("\n\n{}", warnings.join("\n\n")));
    }
    if !ask(hwnd, "Create the 3DS version?", &text) {
        return;
    }
    let o = crate::n3ds::Options { rtp: (!k.rtp.is_empty()).then(|| PathBuf::from(&k.rtp)), ..Default::default() };
    spawn_task("Creating the 3DS version", move |log| {
        let out = crate::n3ds::create(&game, &o, log)?;
        Ok(format!("Made:\n  {}\n\nCopy its folders \"3ds\" and \"mkxpz\" to the top of the 3DS's SD card, then start \
                    MKXP-Z 3DS in the Homebrew Launcher (see README.txt there).", out.display()))
    });
}

fn start_patch(hwnd: HWND) {
    if with(|a| choice(&a.c).system == System::N3ds).unwrap_or(false) {
        start_n3ds(hwnd);
        return;
    }
    let Some((missing, warnings, k)) = with(|a| a.plan.as_ref().map(|p| (p.missing_engines.clone(), p.warnings.clone(), key(&a.c))))
        .flatten() else { return };
    if !missing.is_empty() {
        match engines::check_build_tools(&missing) {
            Ok(()) => {
                let prep = with(|a| engines::build_preparation_note(&options(a, &k).paths)).flatten()
                    .map(|n| format!("\n\n{}", n)).unwrap_or_default();
                let text = format!("These engines aren't built yet:\n  {}\n\nThe first build downloads and compiles mkxp-z \
                                    and all its libraries; that takes roughly 10-30 minutes per engine. Later games reuse \
                                    them.{}", missing.iter().map(|p| p.label()).collect::<Vec<_>>().join("\n  "), prep);
                if ask(hwnd, "Build the missing engines?", &text) {
                    do_patch(missing);
                }
            }
            Err(e) => {
                with(|a| {
                    a.result = Some(Err(format!("The missing engines can't be built here yet.\n\n{}", e)));
                    render_summary(a);
                });
            }
        }
        return;
    }
    if !warnings.is_empty() && !ask(hwnd, "Patch anyway?", &warnings.join("\n\n")) {
        return;
    }
    do_patch(Vec::new());
}

fn do_patch(build: Vec<Platform>) {
    let Some((opts, game)) = with(|a| {
        let k = key(&a.c);
        (options(a, &k), PathBuf::from(&k.game))
    }) else { return };
    let label = if build.is_empty() { "Patching" } else { "Building engines" };
    spawn_task(label, move |log| {
        for p in &build {
            log(format!("Building the {} engine. The first build downloads and compiles all of mkxp-z's libraries \
                         and can take 10-30 minutes.", p.label()));
            engines::build(*p, &opts.paths, log)?;
        }
        let mut plan = Plan::new(&game, &opts)?;
        patch::apply(&mut plan, log)?;
        let names: Vec<String> = plan.launchers().into_iter().map(|(p, n)| format!("  {}: {}", p.label(), n)).collect();
        Ok(format!("Patched \"{}\".\n\nStart it from the game folder with:\n{}", plan.title, names.join("\n")))
    });
}

fn do_unpatch() {
    let Some(game) = with(|a| PathBuf::from(get_text(a.c.edits[0]).trim())) else { return };
    spawn_task("Removing patch", move |log| {
        patch::unpatch(&game, log)?;
        Ok("Patch removed.".into())
    });
}

fn host_launcher(a: &App) -> Option<PathBuf> {
    let plan = a.plan.as_ref()?;
    let m = plan.manifest.as_ref()?;
    if !m.platforms.iter().any(|x| x.starts_with("windows")) {
        return None;
    }
    // Game.exe or Game.mkxp-z(.64).exe/.cmd, as the patch recorded it
    m.files.iter().filter(|f| f.ends_with(".exe") || f.ends_with(".cmd"))
        .map(|n| plan.game_dir.join(n)).find(|p| p.is_file())
}

fn start_game() {
    let Some(l) = with(|a| host_launcher(a)).flatten() else { return };
    let dir = l.parent().map(Path::to_path_buf).unwrap_or_default();
    let is_cmd = l.extension().map_or(false, |e| e.eq_ignore_ascii_case("cmd"));
    let r = if is_cmd {
        std::process::Command::new("cmd").arg("/C").arg("start").arg("").arg(&l).current_dir(&dir).spawn()
    } else {
        std::process::Command::new(&l).current_dir(&dir).spawn()
    };
    if let Err(e) = r {
        with(|a| {
            a.result = Some(Err(format!("Couldn't start the game: {}", e)));
            render_summary(a);
        });
    }
}

fn poll() {
    let mut refresh = false;
    let mut force_analyze = false;
    with(|a| {
        let mut lines = Vec::new();
        while let Ok(m) = a.rx.try_recv() {
            match m {
                Msg::Analyzed(gen, r) => {
                    if let Some((g, k)) = a.pending.take() {
                        if g == gen {
                            a.analyzed = Some(k);
                            match r {
                                Ok(p) => {
                                    a.plan = Some(p);
                                    a.plan_error = None;
                                }
                                Err(e) => {
                                    a.plan = None;
                                    a.plan_error = Some(e);
                                }
                            }
                            refresh = true;
                        } else {
                            a.pending = Some((g, k)); // a newer analysis is still running
                        }
                    }
                }
                Msg::Log(l) => lines.push(l),
                Msg::Finished(r) => {
                    a.busy = None;
                    a.result = Some(r);
                    refresh = true;
                    force_analyze = true;
                }
            }
        }
        if !lines.is_empty() {
            let h = a.c.log;
            unsafe {
                // keep the log bounded during long builds
                if GetWindowTextLengthW(h) > 2_000_000 {
                    let t = get_text(h);
                    set_text(h, &t[t.len() / 2..]);
                }
                let end = GetWindowTextLengthW(h) as usize;
                SendMessageW(h, EM_SETSEL, end, end as LPARAM);
                SendMessageW(h, EM_REPLACESEL, 0, wide(&crlf(&(lines.join("\n") + "\n"))).as_ptr() as LPARAM);
            }
        }
        if refresh {
            render_summary(a);
            update_controls(a);
        }
    });
    if force_analyze {
        analyze(true); // refresh status ("already patched" etc.)
    }
}

/// Lines of the info text with their colour (None = normal text colour).
fn summary_lines(a: &App) -> Vec<(String, Option<u32>)> {
    let mut out: Vec<(String, Option<u32>)> = Vec::new();
    match &a.result {
        Some(Ok(t)) => out.extend(t.lines().map(|l| (l.to_string(), Some(COLOR_OK))).chain([(String::new(), None)])),
        Some(Err(e)) => out.extend(format!("Error: {}", e).lines().map(|l| (l.to_string(), Some(COLOR_ERR)))
                                   .chain([(String::new(), None)])),
        None => {}
    }
    let k = key(&a.c);
    if k.game.is_empty() {
        out.push(("Choose a game folder to begin.".into(), None));
    } else if k.platforms.is_empty() && k.choice.system != System::N3ds {
        out.push(("There's no engine for this system and CPU type.".into(), None));
    } else if let Some(e) = &a.plan_error {
        out.extend(format!("Error: {}", e).lines().map(|l| (l.to_string(), Some(COLOR_ERR))));
    } else if let Some(p) = &a.plan {
        for line in p.summary().lines() {
            let color = if line.starts_with("Warning") {
                Some(COLOR_WARN)
            } else if line.starts_with("Error") || line.starts_with("Missing") {
                Some(COLOR_ERR)
            } else {
                None
            };
            out.push((line.to_string(), color));
        }
    } else if !Path::new(&k.game).is_dir() {
        out.push(("That folder doesn't exist.".into(), None));
    }
    out
}

fn render_summary(a: &mut App) {
    let lines = summary_lines(a);
    let h = a.c.summary;
    if !a.c.rich {
        let text: Vec<&str> = lines.iter().map(|(l, _)| l.as_str()).collect();
        set_text(h, &crlf(&text.join("\n")));
        return;
    }
    set_text(h, "");
    for (i, (line, color)) in lines.iter().enumerate() {
        let mut cf: CHARFORMATW = unsafe { std::mem::zeroed() };
        cf.cbSize = std::mem::size_of::<CHARFORMATW>() as u32;
        cf.dwMask = CFM_COLOR;
        match color {
            Some(c) => cf.crTextColor = *c,
            None => cf.dwEffects = CFE_AUTOCOLOR,
        }
        let text = if i + 1 < lines.len() { format!("{}\r\n", line) } else { line.clone() };
        unsafe {
            let end = GetWindowTextLengthW(h) as usize;
            // RichEdit counts line breaks as one character, GetWindowTextLength as two;
            // a position past the end just lands at the end, which is what we want.
            SendMessageW(h, EM_SETSEL, end, end as LPARAM);
            SendMessageW(h, EM_SETCHARFORMAT, SCF_SELECTION, &cf as *const _ as LPARAM);
            SendMessageW(h, EM_REPLACESEL, 0, wide(&text).as_ptr() as LPARAM);
        }
    }
    unsafe { SendMessageW(h, EM_SETSEL, 0, 0) };
}

fn update_controls(a: &mut App) {
    let c = &a.c;
    let busy = a.busy.is_some();
    for h in c.edits.iter().chain(&c.browse) {
        enable(*h, !busy);
    }
    let system = choice(c).system;
    for (i, h) in c.choice.iter().enumerate() {
        let ok = if i < System::ALL.len() { System::ALL[i] != System::Android }
                 else { system.has(Cpu::ALL[i - System::ALL.len()]) };
        enable(*h, !busy && ok);
    }
    let win = matches!(system, System::Windows | System::Legacy);
    enable(c.soundfont, !busy && win);
    enable(c.replace_exe, !busy && win);
    let plan = a.plan.as_ref();
    set_text(c.patch, match plan {
        _ if system == System::N3ds => "Create 3DS version",
        Some(p) if !p.missing_engines.is_empty() => "Build && patch",
        Some(p) if p.patched() => "Re-patch game",
        _ => "Patch game",
    });
    enable(c.patch, plan.map_or(false, |p| p.errors.is_empty()) && !busy);
    enable(c.unpatch, plan.map_or(false, |p| p.patched()) && !busy);
    enable(c.start, !busy && host_launcher(a).is_some());
    let status = if let Some(b) = &a.busy {
        format!("{} …", b)
    } else if a.pending.is_some() {
        "Analyzing the game …".into()
    } else {
        String::new()
    };
    set_text(c.status, &status);
}

/// Test hook: save the window's client area as a .bmp (MKXPZ_SCREENSHOT=<file>),
/// used to check the layout under Wine. Not used in normal operation.
fn screenshot(hwnd: HWND, path: &Path) {
    unsafe {
        let mut r: RECT = std::mem::zeroed();
        GetClientRect(hwnd, &mut r);
        let (w, h) = (r.right - r.left, r.bottom - r.top);
        let wdc = GetDC(hwnd);
        let mdc = CreateCompatibleDC(wdc);
        let bmp = CreateCompatibleBitmap(wdc, w, h);
        let old = SelectObject(mdc, bmp);
        BitBlt(mdc, 0, 0, w, h, wdc, 0, 0, SRCCOPY);
        SelectObject(mdc, old);
        let mut bi: BITMAPINFO = std::mem::zeroed();
        bi.bmiHeader = BITMAPINFOHEADER { biSize: std::mem::size_of::<BITMAPINFOHEADER>() as u32, biWidth: w,
                                          biHeight: h, biPlanes: 1, biBitCount: 24, biCompression: 0,
                                          ..std::mem::zeroed() };
        let stride = ((w * 3 + 3) & !3) as usize;
        let mut pixels = vec![0u8; stride * h as usize];
        GetDIBits(mdc, bmp, 0, h as u32, pixels.as_mut_ptr() as *mut _, &mut bi, 0);
        DeleteObject(bmp);
        DeleteDC(mdc);
        ReleaseDC(hwnd, wdc);
        let mut file = Vec::with_capacity(54 + pixels.len());
        file.extend_from_slice(b"BM");
        file.extend_from_slice(&((54 + pixels.len()) as u32).to_le_bytes());
        file.extend_from_slice(&[0, 0, 0, 0]);
        file.extend_from_slice(&54u32.to_le_bytes());
        let hdr = std::slice::from_raw_parts(&bi.bmiHeader as *const _ as *const u8, 40);
        file.extend_from_slice(hdr);
        file.extend_from_slice(&pixels);
        let _ = std::fs::write(path, file);
    }
}
