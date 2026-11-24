//! MKXP-Z Patcher: run RPG Maker XP / VX / VX Ace games on the mkxp-z engine.
//! The window uses GTK 3 on Linux and native Win32 controls on Windows.

pub mod android;
pub mod appimage;
pub mod archive;
pub mod engines;
pub mod fonts;
pub mod game;
#[cfg(all(not(windows), feature = "gtk"))]
pub mod gui;
#[cfg(windows)]
pub mod gui_win;
pub mod json;
pub mod linux_sysroot;
pub mod marshal;
pub mod n3ds;
pub mod patch;
pub mod pe;
pub mod plan;
pub mod png;
pub mod srcpatch;
pub mod targz;
pub mod util;

/// Write panics to <data dir>/crash.log (a GUI app has no visible stderr).
pub fn install_crash_log() {
    std::panic::set_hook(Box::new(|info| {
        let thread = std::thread::current().name().unwrap_or("unnamed").to_string();
        let msg = format!("MKXP-Z Patcher {} crashed in thread '{}':\n{}\n\n{}\n",
                          env!("CARGO_PKG_VERSION"), thread, info, std::backtrace::Backtrace::force_capture());
        eprintln!("{}", msg);
        let dir = util::data_dir();
        let _ = std::fs::create_dir_all(&dir);
        let _ = std::fs::write(dir.join("crash.log"), msg);
    }));
}
