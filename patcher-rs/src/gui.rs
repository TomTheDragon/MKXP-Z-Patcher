//! The window, built with the system's GTK 3 (on Windows: GTK from MSYS2).
//! Slow work (analysis, engine builds, patching) runs on worker threads that
//! report back through a channel polled from the GTK main loop.

use crate::engines::{self, Choice, Cpu, Paths, Platform, System};
use crate::patch;
use crate::plan::{Options, Plan};
use gtk::prelude::*;
use gtk::{gdk, glib};
use std::cell::RefCell;
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::sync::mpsc::{channel, Receiver, Sender};

enum Msg {
    Analyzed(u64, Result<Plan, String>),
    Log(String),
    /// background task finished: Ok(message) or Err(error)
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

struct Widgets {
    window: gtk::Window,
    game: gtk::Entry,
    engines: gtk::Entry,
    source: gtk::Entry,
    rtp: gtk::Entry,
    browse: Vec<gtk::Button>,
    /// one per System::ALL
    systems: Vec<gtk::RadioButton>,
    /// one per Cpu::ALL
    cpus: Vec<gtk::RadioButton>,
    soundfont: gtk::CheckButton,
    replace_exe: gtk::CheckButton,
    summary: gtk::TextBuffer,
    log: gtk::TextBuffer,
    log_view: gtk::TextView,
    spinner: gtk::Spinner,
    status: gtk::Label,
    patch: gtk::Button,
    unpatch: gtk::Button,
    start: gtk::Button,
    appimage: gtk::Button,
}

struct State {
    plan: Option<Plan>,
    plan_error: Option<String>,
    analyzed: Option<Key>,
    pending: Option<(u64, Key)>,
    generation: u64,
    busy: Option<String>,
    result: Option<Result<String, String>>,
    debounce: Option<glib::SourceId>,
    chooser: Option<gtk::FileChooserNative>,
    linux_engine: Option<PathBuf>,
    msys2: PathBuf,
}

struct App {
    w: Widgets,
    s: RefCell<State>,
    tx: Sender<Msg>,
}

pub fn run(game: Option<PathBuf>, paths: Paths, linux_engine: Option<PathBuf>) -> Result<(), String> {
    gtk::init().map_err(|e| format!("GTK couldn't start ({}). Is a graphical desktop running?", e))?;
    let (tx, rx) = channel();
    let w = build_widgets();
    let paths = paths.resolved();
    w.game.set_text(&game.map(|g| g.display().to_string()).unwrap_or_default());
    w.engines.set_text(&paths.engines.display().to_string());
    w.source.set_text(&paths.source.display().to_string());
    let host = Choice::for_host();
    set_choice(&w, host);
    let app = Rc::new(App {
        w,
        s: RefCell::new(State {
            plan: None, plan_error: None, analyzed: None, pending: None, generation: 0, busy: None,
            result: None, debounce: None, chooser: None, linux_engine, msys2: paths.msys2.clone(),
        }),
        tx,
    });
    connect(&app);
    poll(&app, rx);
    app.w.window.show_all();
    update_controls(&app);
    render_summary(&app);
    schedule_analyze(&app, 0);
    gtk::main();
    Ok(())
}

fn path_row(grid: &gtk::Grid, row: i32, label: &str, hint: &str) -> (gtk::Entry, gtk::Button) {
    let l = gtk::Label::new(Some(label));
    l.set_xalign(0.0);
    let e = gtk::Entry::new();
    e.set_hexpand(true);
    e.set_placeholder_text(Some(hint));
    let b = gtk::Button::with_label("Browse…");
    grid.attach(&l, 0, row, 1, 1);
    grid.attach(&e, 1, row, 1, 1);
    grid.attach(&b, 2, row, 1, 1);
    (e, b)
}

fn text_view(mono: bool) -> (gtk::TextView, gtk::TextBuffer, gtk::ScrolledWindow) {
    let view = gtk::TextView::new();
    view.set_editable(false);
    view.set_cursor_visible(false);
    view.set_wrap_mode(gtk::WrapMode::WordChar);
    view.set_monospace(mono);
    view.set_left_margin(6);
    view.set_right_margin(6);
    let buf = view.buffer().expect("text buffer");
    let table = buf.tag_table().expect("tag table");
    for (name, color) in [("warn", "#d07000"), ("err", "#d03030"), ("ok", "#2e9e44")] {
        let tag = gtk::TextTag::new(Some(name));
        tag.set_foreground(Some(color));
        table.add(&tag);
    }
    let sw = gtk::ScrolledWindow::new(gtk::Adjustment::NONE, gtk::Adjustment::NONE);
    sw.set_policy(gtk::PolicyType::Automatic, gtk::PolicyType::Automatic);
    sw.add(&view);
    (view, buf, sw)
}

fn build_widgets() -> Widgets {
    let window = gtk::Window::new(gtk::WindowType::Toplevel);
    window.set_title("MKXP-Z Patcher");
    window.set_default_size(1000, 720);
    let vbox = gtk::Box::new(gtk::Orientation::Vertical, 8);
    vbox.set_border_width(12);

    let grid = gtk::Grid::new();
    grid.set_row_spacing(6);
    grid.set_column_spacing(8);
    let (game, game_b) = path_row(&grid, 0, "Game folder", "the folder with Game.exe / Game.ini - type, paste, browse or drop it here");
    let (engines, engines_b) = path_row(&grid, 1, "Engines folder", "built engines (created as needed)");
    let (rtp, rtp_b) = path_row(&grid, 2, "RTP (optional)", "only for games that need the RPG Maker RTP");
    vbox.pack_start(&grid, false, false, 0);

    let expander = gtk::Expander::new(Some("Advanced"));
    let adv = gtk::Grid::new();
    adv.set_row_spacing(6);
    adv.set_column_spacing(8);
    adv.set_margin_top(6);
    let (source, source_b) = path_row(&adv, 0, "mkxp-z source", "mkxp-z source folder");
    let note = gtk::Label::new(Some(&format!("Fetched automatically (commit {}) when an engine has to be built and no \
                                              source is there.", &engines::MKXPZ_COMMIT[..7])));
    note.set_xalign(0.0);
    note.style_context().add_class("dim-label");
    adv.attach(&note, 1, 1, 2, 1);
    expander.add(&adv);
    vbox.pack_start(&expander, false, false, 0);

    let frame = gtk::Frame::new(Some("Patch for"));
    let fbox = gtk::Box::new(gtk::Orientation::Vertical, 4);
    fbox.set_border_width(8);
    let check = |label: &str, tip: &str| {
        let c = gtk::CheckButton::with_label(label);
        if !tip.is_empty() {
            c.set_tooltip_text(Some(tip));
        }
        c
    };
    // radio buttons: one system and one CPU type
    let radios = |items: &[(&str, &str)]| {
        let mut out: Vec<gtk::RadioButton> = Vec::new();
        for (label, tip) in items {
            let r = match out.first() {
                Some(first) => gtk::RadioButton::with_label_from_widget(first, label),
                None => gtk::RadioButton::with_label(label),
            };
            if !tip.is_empty() {
                r.set_tooltip_text(Some(tip));
            }
            out.push(r);
        }
        out
    };
    let systems = radios(&[
        ("Linux", "Built with this computer's compiler against an old C library: runs on Linux systems from \
                   2020 on (glibc 2.31: Ubuntu 20.04, Debian 11, SteamOS 3 and newer). No container needed."),
        ("Linux legacy", "Built in an Ubuntu 20.04 container (needs podman or docker), as earlier versions of the \
                          patcher did. 32-bit and 64-bit only."),
        ("Windows", "Windows 7 and newer."),
        ("Windows legacy (XP)", "Windows XP and newer: only the legacy 32-bit engine (OpenGL only, no HTTPS or \
                                 Ruby sockets), which also runs on newer Windows."),
        ("Android", "An APK of the game for Android 8.0 and newer, next to the game folder (the folder itself \
                     stays as it is). ARM 64-bit: phones and tablets; 64-bit: the Android emulator and Waydroid. \
                     Downloads Google's Android tools (about 1.1 GB) when first needed."),
        ("Nintendo 3DS", "A version for the Nintendo 3DS (homebrew, MKXP-Z 3DS) next to the game folder (the folder \
                          itself stays as it is): the app and the game, its images and sound converted for the 3DS. \
                          Copy it to the SD card; needs custom firmware (Luma3DS) and the Homebrew Launcher."),
    ]);
    let cpus = radios(&[
        ("32-bit", "x86 PCs. For Windows the recommended one: it runs on 32- and 64-bit Windows and loads the game's \
                    own 32-bit DLLs (e.g. Steam's)."),
        ("64-bit", "x86-64 PCs."),
        ("ARM 32-bit", "32-bit ARM (armhf, e.g. older Raspberry Pi systems). Linux only."),
        ("ARM 64-bit", "64-bit ARM (aarch64): Raspberry Pi 4/5, Windows on ARM."),
    ]);
    let soundfont = check("bundle a SoundFont for MIDI music", "");
    soundfont.set_active(true);
    let replace_exe = check("replace Game.exe (the original is kept as Game.original.exe)",
                            "On: the mkxp-z launcher takes the place of Game.exe, so shortcuts and Steam start \
                             mkxp-z unchanged. Off: it's added next to Game.exe as \"Game.mkxp-z.exe\" (64-bit and ARM 64-bit: \"Game.mkxp-z.64.exe\"), and \
                             Steam versions need launch options (see mkxp-z/README.txt).");
    replace_exe.set_active(true);
    let row = gtk::Box::new(gtk::Orientation::Horizontal, 12);
    let heading = |t: &str| {
        let l = gtk::Label::new(Some(t));
        l.style_context().add_class("dim-label");
        l
    };
    row.pack_start(&heading("System:"), false, false, 0);
    for r in &systems {
        row.pack_start(r, false, false, 0);
    }
    row.pack_start(&gtk::Separator::new(gtk::Orientation::Vertical), false, false, 0);
    row.pack_start(&heading("CPU:"), false, false, 0);
    for r in &cpus {
        row.pack_start(r, false, false, 0);
    }
    fbox.pack_start(&row, false, false, 0);
    let row2 = gtk::Box::new(gtk::Orientation::Horizontal, 12);
    row2.pack_start(&soundfont, false, false, 0);
    row2.pack_start(&replace_exe, false, false, 0);
    fbox.pack_start(&row2, false, false, 0);
    let hint = gtk::Label::new(Some("32-bit Windows runs the game's own 32-bit DLLs natively (e.g. Steam's); 64-bit and \
                                     ARM can't load them."));
    hint.set_xalign(0.0);
    hint.set_line_wrap(true);
    hint.style_context().add_class("dim-label");
    fbox.pack_start(&hint, false, false, 0);
    frame.add(&fbox);
    vbox.pack_start(&frame, false, false, 0);

    let (_, summary, summary_sw) = text_view(false);
    let (log_view, log, log_sw) = text_view(true);
    let paned = gtk::Paned::new(gtk::Orientation::Vertical);
    paned.pack1(&summary_sw, true, false);
    paned.pack2(&log_sw, true, false);
    paned.set_position(300);
    vbox.pack_start(&paned, true, true, 0);

    let bottom = gtk::Box::new(gtk::Orientation::Horizontal, 8);
    let spinner = gtk::Spinner::new();
    let status = gtk::Label::new(None);
    let patch = gtk::Button::with_label("Patch game");
    let unpatch = gtk::Button::with_label("Remove patch");
    let start = gtk::Button::with_label("Start game");
    let appimage = gtk::Button::with_label("Create AppImage");
    appimage.set_tooltip_text(Some("Pack the game with its Linux engine into one AppImage file per Linux platform \
                                    chosen, next to the game folder. The game folder itself stays as it is."));
    patch.style_context().add_class("suggested-action");
    bottom.pack_start(&spinner, false, false, 0);
    bottom.pack_start(&status, false, false, 0);
    bottom.pack_end(&patch, false, false, 0);
    bottom.pack_end(&unpatch, false, false, 0);
    bottom.pack_end(&start, false, false, 0);
    bottom.pack_end(&appimage, false, false, 0);
    vbox.pack_start(&bottom, false, false, 0);
    window.add(&vbox);

    Widgets { window, game, engines, source, rtp, browse: vec![game_b, engines_b, rtp_b, source_b], systems, cpus,
              soundfont, replace_exe, summary, log, log_view, spinner, status, patch, unpatch,
              start, appimage }
}

fn choice(w: &Widgets) -> Choice {
    let pick = |rs: &[gtk::RadioButton]| rs.iter().position(|r| r.is_active()).unwrap_or(0);
    Choice { system: System::ALL[pick(&w.systems)], cpu: Cpu::ALL[pick(&w.cpus)] }
}

fn set_choice(w: &Widgets, c: Choice) {
    if let Some(i) = System::ALL.iter().position(|s| *s == c.system) {
        w.systems[i].set_active(true);
    }
    if let Some(i) = Cpu::ALL.iter().position(|x| *x == c.cpu) {
        w.cpus[i].set_active(true);
    }
}

fn key(w: &Widgets) -> Key {
    let choice = choice(w);
    let platforms = choice.platforms();
    Key { game: w.game.text().trim().to_string(), engines: w.engines.text().trim().to_string(),
          source: w.source.text().trim().to_string(), rtp: w.rtp.text().trim().to_string(),
          choice, platforms, soundfont: w.soundfont.is_active(), replace_exe: w.replace_exe.is_active() }
}

fn options(app: &App, k: &Key) -> Options {
    let s = app.s.borrow();
    Options {
        platforms: k.platforms.clone(),
        paths: Paths { engines: PathBuf::from(&k.engines), source: PathBuf::from(&k.source), msys2: s.msys2.clone() },
        linux_engine: s.linux_engine.clone(),
        rtp: (!k.rtp.is_empty()).then(|| PathBuf::from(&k.rtp)),
        rgss: None,
        bundle_soundfont: k.soundfont,
        replace_exe: k.replace_exe,
        n3ds: k.choice.system == System::N3ds,
    }
}

fn connect(app: &Rc<App>) {
    let w = &app.w;
    w.window.connect_destroy(|_| gtk::main_quit());

    let fields = [(&w.game, "Select the RPG Maker game folder"), (&w.engines, "Select the engines folder"),
                  (&w.rtp, "Select the RTP folder"), (&w.source, "Select the mkxp-z source folder")];
    for ((entry, title), button) in fields.into_iter().zip(&w.browse) {
        let a = app.clone();
        entry.connect_changed(move |e| {
            let t = e.text().to_string();
            if t.starts_with("file://") || t.starts_with('"') || t.starts_with('\'') {
                e.set_text(&crate::util::clean_path(&t)); // triggers "changed" again with the clean text
                return;
            }
            e.set_tooltip_text((!t.is_empty()).then_some(t.as_str()));
            schedule_analyze(&a, 300);
        });
        let a = app.clone();
        let target = entry.clone();
        button.connect_clicked(move |_| {
            let d = gtk::FileChooserNative::new(Some(title), Some(&a.w.window), gtk::FileChooserAction::SelectFolder,
                                                Some("Choose"), Some("Cancel"));
            let cur = target.text().to_string();
            if Path::new(&cur).is_dir() {
                let _ = d.set_current_folder(&cur);
            }
            let (a2, t2) = (a.clone(), target.clone());
            d.connect_response(move |d, r| {
                if r == gtk::ResponseType::Accept {
                    if let Some(p) = d.filename() {
                        t2.set_text(&p.display().to_string());
                    }
                }
                a2.s.borrow_mut().chooser = None;
            });
            d.show();
            a.s.borrow_mut().chooser = Some(d); // keep it alive until it answers
        });
    }
    for r in &w.systems {
        let a = app.clone();
        r.connect_toggled(move |r| {
            if r.is_active() {
                // a CPU type the new system has no engine for: 32-bit instead
                let c = choice(&a.w);
                if c.fixed() != c {
                    set_choice(&a.w, c.fixed());
                }
            }
            update_controls(&a);
            schedule_analyze(&a, 0);
        });
    }
    let toggles: Vec<gtk::CheckButton> = w.cpus.iter().map(|r| r.clone().upcast())
        .chain([w.soundfont.clone(), w.replace_exe.clone()]).collect();
    for c in &toggles {
        let a = app.clone();
        c.connect_toggled(move |_| {
            update_controls(&a);
            schedule_analyze(&a, 0);
        });
    }

    // a game folder (or a file in it, e.g. Game.exe) dropped onto the window
    let targets = [gtk::TargetEntry::new("text/uri-list", gtk::TargetFlags::OTHER_APP, 0)];
    w.window.drag_dest_set(gtk::DestDefaults::ALL, &targets, gdk::DragAction::COPY);
    let a = app.clone();
    w.window.connect_drag_data_received(move |_, _, _, _, data, _, _| {
        if a.s.borrow().busy.is_some() {
            return;
        }
        if let Some(uri) = data.uris().first() {
            if let Ok((p, _)) = glib::filename_from_uri(uri) {
                let dir = if p.is_dir() { p } else { p.parent().map(Path::to_path_buf).unwrap_or(p) };
                a.w.game.set_text(&dir.display().to_string());
            }
        }
    });

    let a = app.clone();
    w.patch.connect_clicked(move |_| start_patch(&a));
    let a = app.clone();
    w.unpatch.connect_clicked(move |_| {
        let game = a.w.game.text().to_string();
        let a2 = a.clone();
        confirm(&a, "Remove the patch?", &format!("Remove the mkxp-z files from\n{}?\n\nGame files and save games are \
                                                  not touched.", game.trim()), move || do_unpatch(&a2));
    });
    let a = app.clone();
    w.start.connect_clicked(move |_| start_game(&a));
    let a = app.clone();
    w.appimage.connect_clicked(move |_| start_appimage(&a));
}

/// Linux platforms chosen (AppImages are made for these).
fn appimage_platforms(app: &App) -> Vec<Platform> {
    key(&app.w).platforms.into_iter().filter(|p| crate::appimage::arch(*p).is_some()).collect()
}

fn start_appimage(app: &Rc<App>) {
    if let Err(e) = crate::appimage::check_tools() {
        app.s.borrow_mut().result = Some(Err(e));
        render_summary(app);
        return;
    }
    let linux = appimage_platforms(app);
    let missing: Vec<Platform> = app.s.borrow().plan.as_ref()
        .map(|p| p.missing_engines.iter().copied().filter(|m| linux.contains(m)).collect()).unwrap_or_default();
    if !missing.is_empty() {
        if let Err(e) = engines::check_build_tools(&missing) {
            app.s.borrow_mut().result = Some(Err(format!("The missing engines can't be built here yet.\n\n{}", e)));
            render_summary(app);
            return;
        }
    }
    let k = key(&app.w);
    let opts = Options { platforms: linux.clone(), ..options(app, &k) };
    let game = PathBuf::from(&k.game);
    let outs: Vec<String> = app.s.borrow().plan.as_ref().map(|p| linux.iter()
        .map(|l| crate::appimage::output_path(&game, &p.title, *l).display().to_string()).collect()).unwrap_or_default();
    let a = app.clone();
    confirm(app, "Create AppImage?", &format!(
        "This makes:\n  {}\n\nThe game folder stays as it is. Saves made with an AppImage go to \
         ~/.local/share/<game title>/game, its settings to ~/.config/<game title>.{}", outs.join("\n  "),
        if missing.is_empty() { String::new() } else {
            format!("\n\nFirst these engines are built (10-30 minutes each): {}",
                    missing.iter().map(|p| p.label()).collect::<Vec<_>>().join(", "))
        }),
        move || {
            let (opts, game, missing) = (opts.clone(), game.clone(), missing.clone());
            spawn_task(&a, "Creating AppImage", move |log| {
                for p in &missing {
                    log(format!("Building the {} engine first (10-30 minutes) ...", p.label()));
                    engines::build(*p, &opts.paths, log)?;
                }
                let made = crate::appimage::create(&game, &opts, log)?;
                Ok(format!("Made:\n  {}\n\nCopy it anywhere and start it like a program. Saves go to \
                            ~/.local/share/<game title>/game, settings to ~/.config/<game title>.",
                           made.iter().map(|m| m.display().to_string()).collect::<Vec<_>>().join("\n  ")))
            });
        });
}

/// Android: the game goes into an APK next to its folder (android.rs).
fn start_apk(app: &Rc<App>) {
    if let Err(e) = crate::android::check_apk_tools() {
        app.s.borrow_mut().result = Some(Err(e));
        render_summary(app);
        return;
    }
    let k = key(&app.w);
    let opts = options(app, &k);
    let missing: Vec<Platform> = app.s.borrow().plan.as_ref().map(|p| p.missing_engines.clone()).unwrap_or_default();
    if !missing.is_empty() {
        if let Err(e) = engines::check_build_tools(&missing) {
            app.s.borrow_mut().result = Some(Err(format!("The missing engines can't be built here yet.\n\n{}", e)));
            render_summary(app);
            return;
        }
    }
    let game = PathBuf::from(&k.game);
    let out = app.s.borrow().plan.as_ref()
        .map(|p| crate::android::output_path(&game, &p.title).display().to_string()).unwrap_or_default();
    let tools = if crate::android::tools_dir(&opts.paths).join("android-ndk-r29").is_dir() { String::new() } else {
        format!("\n\nFirst Google's Android tools are downloaded into {} (about 1.1 GB, 3.9 GB unpacked).",
                crate::android::tools_dir(&opts.paths).display())
    };
    let a = app.clone();
    confirm(app, "Create APK?", &format!(
        "This makes:\n  {}\n\nThe game folder stays as it is.{}{}", out, tools,
        if missing.is_empty() { String::new() } else {
            format!("\n\nThen these engines are built (10-30 minutes each): {}",
                    missing.iter().map(|p| p.label()).collect::<Vec<_>>().join(", "))
        }),
        move || {
            let (opts, game, missing) = (opts.clone(), game.clone(), missing.clone());
            spawn_task(&a, "Creating APK", move |log| {
                for p in &missing {
                    log(format!("Building the {} engine first (10-30 minutes) ...", p.label()));
                    engines::build(*p, &opts.paths, log)?;
                }
                let made = crate::android::create(&game, &opts, log)?;
                Ok(format!("Made:\n  {}\n\nCopy it to the Android device and open it there to install it (allow \
                            installing apps from that source), or: adb install -r \"{}\"",
                           made.display(), made.display()))
            });
        });
}

fn confirm(app: &Rc<App>, title: &str, text: &str, yes: impl Fn() + 'static) {
    let d = gtk::MessageDialog::new(Some(&app.w.window), gtk::DialogFlags::MODAL | gtk::DialogFlags::DESTROY_WITH_PARENT,
                                    gtk::MessageType::Question, gtk::ButtonsType::YesNo, title);
    d.set_secondary_text(Some(text));
    d.connect_response(move |d, r| {
        d.close();
        if r == gtk::ResponseType::Yes {
            yes();
        }
    });
    d.show();
}

fn schedule_analyze(app: &Rc<App>, delay_ms: u64) {
    if let Some(id) = app.s.borrow_mut().debounce.take() {
        id.remove();
    }
    let a = app.clone();
    let id = glib::timeout_add_local_once(std::time::Duration::from_millis(delay_ms), move || {
        a.s.borrow_mut().debounce = None;
        analyze(&a, false);
    });
    app.s.borrow_mut().debounce = Some(id);
}

/// (Re)analyze when the inputs changed and the game folder exists.
fn analyze(app: &Rc<App>, force: bool) {
    let k = key(&app.w);
    let gen = {
        let mut s = app.s.borrow_mut();
        if s.busy.is_some() {
            return;
        }
        if !force && (s.analyzed.as_ref() == Some(&k) || s.pending.as_ref().map(|(_, p)| p) == Some(&k)) {
            return;
        }
        if k.game.is_empty() || !Path::new(&k.game).is_dir() || k.platforms.is_empty() && k.choice.system != System::N3ds {
            s.plan = None;
            s.plan_error = None;
            s.analyzed = Some(k);
            drop(s);
            render_summary(app);
            update_controls(app);
            return;
        }
        s.generation += 1;
        s.pending = Some((s.generation, k.clone()));
        s.generation
    };
    let opts = options(app, &k);
    let tx = app.tx.clone();
    std::thread::spawn(move || {
        let _ = tx.send(Msg::Analyzed(gen, Plan::new(Path::new(&k.game), &opts)));
    });
    update_controls(app);
}

fn spawn_task(app: &Rc<App>, label: &str, f: impl FnOnce(&(dyn Fn(String) + Sync)) -> Result<String, String> + Send + 'static) {
    {
        let mut s = app.s.borrow_mut();
        s.busy = Some(label.into());
        s.result = None;
    }
    app.w.log.set_text("");
    update_controls(app);
    render_summary(app);
    let tx = app.tx.clone();
    std::thread::spawn(move || {
        let log_tx = std::sync::Mutex::new(tx.clone());
        let log = move |s: String| {
            let _ = log_tx.lock().map(|t| t.send(Msg::Log(s)));
        };
        let r = f(&log);
        let _ = tx.send(Msg::Finished(r));
    });
}

/// the Nintendo 3DS version: made next to the game folder (n3ds.rs)
fn start_n3ds(app: &Rc<App>) {
    let k = key(&app.w);
    let game = PathBuf::from(&k.game);
    let name = game.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    let warnings = app.s.borrow().plan.as_ref().map(|p| p.warnings.clone()).unwrap_or_default();
    let o = crate::n3ds::Options { rtp: (!k.rtp.is_empty()).then(|| PathBuf::from(&k.rtp)), ..Default::default() };
    let a = app.clone();
    confirm(app, "Create the 3DS version?", &format!(
        "This makes \"{} (3DS)\" next to the game folder (which stays as it is): the app MKXP-Z 3DS and the game, its \
         images and sound converted for the 3DS. That takes a minute or two.{}", name,
        if warnings.is_empty() { String::new() } else { format!("\n\n{}", warnings.join("\n\n")) }),
        move || {
            let (game, o) = (game.clone(), o.clone());
            spawn_task(&a, "Creating the 3DS version", move |log| {
                let out = crate::n3ds::create(&game, &o, log)?;
                Ok(format!("Made:\n  {}\n\nCopy its folders \"3ds\" and \"mkxpz\" to the top of the 3DS's SD card, \
                            then start MKXP-Z 3DS in the Homebrew Launcher (see README.txt there).", out.display()))
            });
        });
}

fn start_patch(app: &Rc<App>) {
    if choice(&app.w).system == System::Android {
        start_apk(app);
        return;
    }
    if choice(&app.w).system == System::N3ds {
        start_n3ds(app);
        return;
    }
    let (missing, warnings) = match &app.s.borrow().plan {
        Some(p) => (p.missing_engines.clone(), p.warnings.clone()),
        None => return,
    };
    if !missing.is_empty() {
        match engines::check_build_tools(&missing) {
            Ok(()) => {
                let k = key(&app.w);
                let prep = engines::build_preparation_note(&options(app, &k).paths)
                    .map(|n| format!("\n\n{}", n)).unwrap_or_default();
                let a = app.clone();
                let m2 = missing.clone();
                confirm(app, "Build the missing engines?", &format!(
                    "These engines aren't built yet:\n  {}\n\nThe first build downloads and compiles mkxp-z and all its \
                     libraries; that takes roughly 10-30 minutes per engine. Later games reuse them.{}",
                    missing.iter().map(|p| p.label()).collect::<Vec<_>>().join("\n  "), prep),
                        move || do_patch(&a, m2.clone()));
            }
            Err(e) => {
                app.s.borrow_mut().result = Some(Err(format!("The missing engines can't be built here yet.\n\n{}", e)));
                render_summary(app);
            }
        }
        return;
    }
    if !warnings.is_empty() {
        let a = app.clone();
        confirm(app, "Patch anyway?", &warnings.join("\n\n"), move || do_patch(&a, Vec::new()));
        return;
    }
    do_patch(app, Vec::new());
}

/// Build `build` engines (if any), re-analyze, then patch.
fn do_patch(app: &Rc<App>, build: Vec<Platform>) {
    let k = key(&app.w);
    let opts = options(app, &k);
    let game = PathBuf::from(&k.game);
    let label = if build.is_empty() { "Patching" } else { "Building engines" };
    spawn_task(app, label, move |log| {
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

fn do_unpatch(app: &Rc<App>) {
    let game = PathBuf::from(app.w.game.text().trim());
    spawn_task(app, "Removing patch", move |log| {
        patch::unpatch(&game, log)?;
        Ok("Patch removed.".into())
    });
}

fn host_launcher(app: &App) -> Option<PathBuf> {
    let s = app.s.borrow();
    let plan = s.plan.as_ref()?;
    let m = plan.manifest.as_ref()?;
    let names: Vec<String> = if cfg!(windows) && m.platforms.iter().any(|k| k.starts_with("windows")) {
        // the Windows launcher: Game.exe or Game.mkxp-z(.64).exe/.cmd, as the patch recorded it
        m.files.iter().filter(|f| f.ends_with(".exe") || f.ends_with(".cmd")).cloned().collect()
    } else if cfg!(target_os = "linux") && m.platforms.iter().any(|k| k.starts_with("linux")) {
        vec![format!("{}.sh", plan.exec_name)]
    } else {
        return None;
    };
    names.into_iter().map(|n| plan.game_dir.join(n)).find(|p| p.is_file())
}

fn start_game(app: &Rc<App>) {
    let Some(l) = host_launcher(app) else { return };
    let dir = l.parent().map(Path::to_path_buf).unwrap_or_default();
    let is_cmd = l.extension().map_or(false, |e| e.eq_ignore_ascii_case("cmd"));
    let r = if is_cmd {
        std::process::Command::new("cmd").arg("/C").arg("start").arg("").arg(&l).current_dir(&dir).spawn()
    } else {
        std::process::Command::new(&l).current_dir(&dir).spawn()
    };
    if let Err(e) = r {
        app.s.borrow_mut().result = Some(Err(format!("Couldn't start the game: {}", e)));
        render_summary(app);
    }
}

/// Deliver worker messages on the GTK main loop.
fn poll(app: &Rc<App>, rx: Receiver<Msg>) {
    let a = app.clone();
    glib::timeout_add_local(std::time::Duration::from_millis(100), move || {
        let mut summary_dirty = false;
        let mut log_lines = Vec::new();
        while let Ok(m) = rx.try_recv() {
            match m {
                Msg::Analyzed(gen, r) => {
                    let mut s = a.s.borrow_mut();
                    if let Some((g, k)) = s.pending.take() {
                        if g == gen {
                            s.analyzed = Some(k);
                            match r {
                                Ok(p) => {
                                    s.plan = Some(p);
                                    s.plan_error = None;
                                }
                                Err(e) => {
                                    s.plan = None;
                                    s.plan_error = Some(e);
                                }
                            }
                            summary_dirty = true;
                        } else {
                            s.pending = Some((g, k)); // a newer analysis is still running
                        }
                    }
                }
                Msg::Log(l) => log_lines.push(l),
                Msg::Finished(r) => {
                    {
                        let mut s = a.s.borrow_mut();
                        s.busy = None;
                        s.result = Some(r);
                    }
                    summary_dirty = true;
                    analyze(&a, true); // refresh status ("already patched" etc.)
                }
            }
        }
        if !log_lines.is_empty() {
            let buf = &a.w.log;
            let mut end = buf.end_iter();
            buf.insert(&mut end, &(log_lines.join("\n") + "\n"));
            // keep the log from growing without bounds during long builds
            if buf.line_count() > 6000 {
                let (mut s, mut e) = (buf.start_iter(), buf.iter_at_line(buf.line_count() - 5000));
                buf.delete(&mut s, &mut e);
            }
            if let Some(mark) = buf.create_mark(None, &buf.end_iter(), false) {
                a.w.log_view.scroll_to_mark(&mark, 0.0, false, 0.0, 1.0);
                buf.delete_mark(&mark);
            }
        }
        if summary_dirty {
            render_summary(&a);
            update_controls(&a);
        }
        glib::ControlFlow::Continue
    });
}

fn render_summary(app: &App) {
    let buf = &app.w.summary;
    buf.set_text("");
    let put = |text: &str, tag: Option<&str>| {
        let start = buf.end_iter().offset();
        buf.insert(&mut buf.end_iter(), text);
        if let Some(t) = tag {
            buf.apply_tag_by_name(t, &buf.iter_at_offset(start), &buf.end_iter());
        }
    };
    let s = app.s.borrow();
    match &s.result {
        Some(Ok(t)) => put(&format!("{}\n\n", t), Some("ok")),
        Some(Err(e)) => put(&format!("Error: {}\n\n", e), Some("err")),
        None => {}
    }
    let k = key(&app.w);
    if k.game.is_empty() {
        put("Choose a game folder to begin.\n", None);
    } else if k.platforms.is_empty() && k.choice.system != System::N3ds {
        put("There's no engine for this system and CPU type.\n", None);
    } else if let Some(e) = &s.plan_error {
        put(&format!("Error: {}\n", e), Some("err"));
    } else if let Some(p) = &s.plan {
        for line in p.summary().lines() {
            let tag = if line.starts_with("Warning") {
                Some("warn")
            } else if line.starts_with("Error") || line.starts_with("Missing") {
                Some("err")
            } else {
                None
            };
            put(&format!("{}\n", line), tag);
        }
    } else if !Path::new(&k.game).is_dir() {
        put("That folder doesn't exist.\n", None);
    }
}

fn update_controls(app: &App) {
    let w = &app.w;
    let android = choice(w).system == System::Android;
    let (busy, pending, patch_label, can_patch, can_unpatch, status) = {
        let s = app.s.borrow();
        let plan = s.plan.as_ref();
        (s.busy.is_some(), s.pending.is_some(),
         match plan {
             _ if android && plan.map_or(false, |p| !p.missing_engines.is_empty()) => "Build engine & create APK",
             _ if android => "Create APK",
             _ if choice(w).system == System::N3ds => "Create 3DS version",
             Some(p) if !p.missing_engines.is_empty() => "Build engines & patch",
             Some(p) if p.patched() => "Re-patch game",
             _ => "Patch game",
         },
         plan.map_or(false, |p| p.errors.is_empty()), plan.map_or(false, |p| p.patched()), s.busy.clone())
    };
    for e in [&w.game, &w.engines, &w.rtp, &w.source] {
        e.set_sensitive(!busy);
    }
    for b in &w.browse {
        b.set_sensitive(!busy);
    }
    let system = choice(w).system;
    for r in &w.systems {
        r.set_sensitive(!busy);
    }
    for (r, cpu) in w.cpus.iter().zip(Cpu::ALL) {
        r.set_sensitive(!busy && system.has(cpu));
    }
    w.appimage.set_sensitive(!busy && can_patch && !appimage_platforms(app).is_empty());
    let win = matches!(system, System::Windows | System::Legacy);
    w.soundfont.set_sensitive(!busy && !matches!(system, System::Linux | System::LinuxLegacy | System::N3ds));
    w.replace_exe.set_sensitive(!busy && win);
    w.patch.set_label(patch_label);
    w.patch.set_sensitive(can_patch && !busy);
    w.unpatch.set_sensitive(can_unpatch && !busy);
    w.start.set_sensitive(!busy && host_launcher(app).is_some());
    if let Some(b) = status {
        w.spinner.start();
        w.status.set_text(&format!("{} …", b));
    } else if pending {
        w.spinner.start();
        w.status.set_text("Analyzing the game …");
    } else {
        w.spinner.stop();
        w.status.set_text("");
    }
}
