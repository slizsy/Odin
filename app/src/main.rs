// odin, the desktop app. all the real work is in odin-core, this is the window
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod files;
mod theme;

use eframe::egui;
use odin_core::{clean_file, inspect, scan, CleanOptions, RenameMode, Report};
use std::path::PathBuf;
use std::sync::mpsc::{channel, Receiver, Sender};
use std::sync::{Arc, Mutex};

pub struct Cleaned {
    pub out: PathBuf,
    pub after: Report,
    pub removed: usize,
}

pub struct Item {
    pub id: u64,
    pub path: PathBuf,
    pub name: String,
    pub report: Option<Report>, // none while it's still being scanned
    pub result: Option<Result<Cleaned, String>>,
    pub busy: bool,
}

pub enum Msg {
    Scanned(u64, Report),
    Cleaned(u64, Result<Cleaned, String>),
}

pub struct Odin {
    pub items: Vec<Item>,
    pub selected: Option<u64>,
    next_id: u64,
    tx: Sender<Msg>,
    rx: Receiver<Msg>,
    pub opts: CleanOptions,
    pub rename: RenameMode,
    pub out_dir: Option<PathBuf>, // none = an odin-clean folder next to each file
    pub show_cleaned: bool,
    dev: Dev,
}

impl Odin {
    fn new(cc: &eframe::CreationContext<'_>, start: Vec<PathBuf>) -> Self {
        theme::apply(&cc.egui_ctx);
        let (tx, rx) = channel();
        let mut app = Odin {
            items: vec![],
            selected: None,
            next_id: 1,
            tx,
            rx,
            opts: CleanOptions::default(),
            rename: RenameMode::Keep,
            out_dir: None,
            show_cleaned: false,
            dev: Dev::from_env(),
        };
        app.add_paths(&cc.egui_ctx, start);
        app
    }

    // files, folders, whatever got dropped. scanning happens off the ui thread
    pub fn add_paths(&mut self, ctx: &egui::Context, paths: Vec<PathBuf>) {
        let mut batch = vec![];
        for path in scan::collect(&paths) {
            if self.items.iter().any(|i| i.path == path) {
                continue;
            }
            let id = self.next_id;
            self.next_id += 1;
            let name = path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
            self.items.push(Item { id, path: path.clone(), name, report: None, result: None, busy: false });
            batch.push((id, path));
        }
        if self.selected.is_none() {
            self.selected = self.items.first().map(|i| i.id);
        }
        if batch.is_empty() {
            return;
        }
        let (tx, ctx) = (self.tx.clone(), ctx.clone());
        std::thread::spawn(move || {
            for (id, path) in batch {
                if tx.send(Msg::Scanned(id, inspect(&path))).is_err() {
                    break;
                }
                ctx.request_repaint();
            }
        });
    }

    // one worker goes through the list in order, so ffmpeg jobs don't pile up
    pub fn clean(&mut self, ctx: &egui::Context, ids: Vec<u64>) {
        let mut jobs = vec![];
        for item in self.items.iter_mut().filter(|i| ids.contains(&i.id) && !i.busy && i.report.is_some()) {
            item.busy = true;
            let dir = self.out_dir.clone().unwrap_or_else(|| item.path.parent().unwrap_or(&item.path).join(scan::OUT_DIR));
            jobs.push((item.id, item.path.clone(), dir));
        }
        if jobs.is_empty() {
            return;
        }
        let (tx, ctx, opts, mode) = (self.tx.clone(), ctx.clone(), self.opts.clone(), self.rename);
        std::thread::spawn(move || {
            for (id, path, dir) in jobs {
                let res = clean_file(&path, &dir, mode, &opts).map(|o| Cleaned { removed: o.removed(), out: o.out, after: o.after }).map_err(|e| e.to_string());
                if tx.send(Msg::Cleaned(id, res)).is_err() {
                    break;
                }
                ctx.request_repaint();
            }
        });
    }

    fn pump(&mut self) {
        while let Ok(msg) = self.rx.try_recv() {
            match msg {
                Msg::Scanned(id, rep) => {
                    if let Some(i) = self.items.iter_mut().find(|i| i.id == id) {
                        i.report = Some(rep);
                    }
                }
                Msg::Cleaned(id, res) => {
                    if let Some(i) = self.items.iter_mut().find(|i| i.id == id) {
                        i.busy = false;
                        i.result = Some(res);
                    }
                }
            }
        }
    }

    pub fn idle(&self) -> bool {
        self.items.iter().all(|i| i.report.is_some() && !i.busy)
    }
}

impl eframe::App for Odin {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        self.pump();
        let ctx = ui.ctx().clone();
        let dropped: Vec<PathBuf> = ctx.input(|i| i.raw.dropped_files.iter().map(|f| f.path().to_path_buf()).collect());
        if !dropped.is_empty() {
            self.add_paths(&ctx, dropped);
        }
        files::show(self, ui);
        self.dev.tick(self.idle(), &ctx);
        if let Some(ids) = self.dev.wants_clean(self.idle(), &self.items) {
            self.clean(&ctx, ids);
        }
    }
}

// ---- dev hook: ODIN_SHOT=out.png renders the window to a png and quits, so the ui can
// be checked on a machine with no screen (ci). ODIN_AUTOCLEAN=1 hits "clean all" first,
// ODIN_SELECT=n picks the nth file. normal runs never touch any of this
struct Dev {
    shot: Option<PathBuf>,
    autoclean: bool,
    cleaned: bool,
    select: Option<usize>,
    calm_frames: u32,
    asked: bool,
    image: Arc<Mutex<Option<Arc<egui::ColorImage>>>>,
}

impl Dev {
    fn from_env() -> Self {
        Dev {
            shot: std::env::var_os("ODIN_SHOT").map(PathBuf::from),
            autoclean: std::env::var_os("ODIN_AUTOCLEAN").is_some(),
            cleaned: false,
            select: std::env::var("ODIN_SELECT").ok().and_then(|s| s.parse().ok()),
            calm_frames: 0,
            asked: false,
            image: Arc::new(Mutex::new(None)),
        }
    }

    fn wants_clean(&mut self, idle: bool, items: &[Item]) -> Option<Vec<u64>> {
        if self.shot.is_some() && self.autoclean && !self.cleaned && idle && !items.is_empty() {
            self.cleaned = true;
            self.calm_frames = 0;
            return Some(items.iter().map(|i| i.id).collect());
        }
        None
    }

    fn tick(&mut self, idle: bool, ctx: &egui::Context) {
        let Some(path) = self.shot.clone() else { return };
        ctx.request_repaint();
        if let Some(img) = self.image.lock().unwrap().take() {
            let raw: Vec<u8> = img.pixels.iter().flat_map(|p| p.to_array()).collect();
            let _ = image::save_buffer(&path, &raw, img.size[0] as u32, img.size[1] as u32, image::ColorType::Rgba8);
            ctx.send_viewport_cmd(egui::ViewportCommand::Close);
            return;
        }
        let ready = idle && (!self.autoclean || self.cleaned);
        self.calm_frames = if ready { self.calm_frames + 1 } else { 0 };
        if self.calm_frames > 12 && !self.asked {
            self.asked = true;
            let slot = self.image.clone();
            ctx.send_viewport_cmd(egui::ViewportCommand::Screenshot(egui::ScreenshotCallback::new(move |img| {
                *slot.lock().unwrap() = Some(img);
            })));
        }
    }
}

fn main() -> eframe::Result {
    // anything on the command line gets loaded on start, so "open with odin" works
    let start: Vec<PathBuf> = std::env::args_os().skip(1).map(PathBuf::from).collect();
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default().with_title("Odin").with_inner_size([1180.0, 760.0]).with_min_inner_size([860.0, 560.0]).with_drag_and_drop(true),
        ..Default::default()
    };
    eframe::run_native(
        "Odin",
        options,
        Box::new(move |cc| {
            let mut app = Odin::new(cc, start);
            if let Some(n) = app.dev.select {
                app.selected = app.items.get(n).map(|i| i.id);
            }
            Ok(Box::new(app))
        }),
    )
}
