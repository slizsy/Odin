// the files screen: list on the left, what's inside the selected file in the middle,
// clean options along the bottom

use crate::theme::*;
use crate::{Item, Odin};
use eframe::egui::{self, Align, Color32, Layout, RichText};
use odin_core::formats::media;
use odin_core::{Group, RenameMode, Report, Risk};
use std::path::Path;

pub fn show(app: &mut Odin, ui: &mut egui::Ui) {
    let ctx = ui.ctx().clone();
    top_bar(app, ui, &ctx);
    bottom_bar(app, ui, &ctx);
    file_list(app, ui);
    egui::CentralPanel::default().frame(egui::Frame::new().fill(BG).inner_margin(egui::Margin::same(18))).show(ui, |ui| viewer(app, ui));
    drop_overlay(&ctx);
}

fn top_bar(app: &mut Odin, ui: &mut egui::Ui, ctx: &egui::Context) {
    egui::Panel::top("top").show(ui, |ui| {
        ui.add_space(8.0);
        ui.horizontal(|ui| {
            ui.label(RichText::new("ODIN").size(22.0).strong().color(ACCENT));
            ui.label(RichText::new("see what your files leak, then strip it").color(DIM));
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                if ui.button("clear list").clicked() {
                    app.items.retain(|i| i.busy);
                    app.selected = None;
                }
                if ui.button("add folder").clicked() {
                    if let Some(dir) = rfd::FileDialog::new().pick_folder() {
                        app.add_paths(ctx, vec![dir]);
                    }
                }
                if ui.button("add files").clicked() {
                    if let Some(files) = rfd::FileDialog::new().pick_files() {
                        app.add_paths(ctx, files);
                    }
                }
                ui.add_space(10.0);
                if media::ffmpeg().is_some() {
                    ui.label(RichText::new("ffmpeg found").small().color(GREEN)).on_hover_text("video and audio cleaning is on");
                } else {
                    // the one outside link on this screen, and it only opens if you click it
                    if ui.link("get ffmpeg").on_hover_text("opens ffmpeg.org in your browser").clicked() {
                        ctx.open_url(egui::OpenUrl::new_tab("https://ffmpeg.org/download.html"));
                    }
                    ui.label(RichText::new("no ffmpeg on this machine, so video + audio can't be cleaned yet").small().color(AMBER));
                }
            });
        });
        ui.add_space(8.0);
    });
}

fn bottom_bar(app: &mut Odin, ui: &mut egui::Ui, ctx: &egui::Context) {
    egui::Panel::bottom("bottom").show(ui, |ui| {
        ui.add_space(10.0);
        ui.horizontal_wrapped(|ui| {
            ui.label(RichText::new("file names").color(DIM));
            ui.radio_value(&mut app.rename, RenameMode::Keep, "keep");
            ui.radio_value(&mut app.rename, RenameMode::Random, "random").on_hover_text("like a3f91c0b72de.jpg");
            ui.radio_value(&mut app.rename, RenameMode::Generic, "generic").on_hover_text("like image-001.jpg, video-001.mp4, document-001.pdf");
            ui.add_space(14.0);
            ui.checkbox(&mut app.opts.keep_icc, "keep colour profile").on_hover_text("leave the icc profile in photos so colours don't shift. off = strip that too");
            ui.checkbox(&mut app.opts.remove_comments, "remove comments").on_hover_text("comments and notes in word, excel, powerpoint and pdf files");
        });
        ui.add_space(2.0);
        ui.horizontal(|ui| {
            ui.label(RichText::new("save to").color(DIM));
            let custom = app.out_dir.is_some();
            if ui.radio(!custom, "an odin-clean folder next to each file").clicked() {
                app.out_dir = None;
            }
            if ui.radio(custom, "one folder...").clicked() {
                if let Some(dir) = rfd::FileDialog::new().pick_folder() {
                    app.out_dir = Some(dir);
                }
            }
            if let Some(dir) = &app.out_dir {
                ui.label(RichText::new(dir.display().to_string()).small().color(DIM));
            }
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                // "all" skips files that already have a clean copy
                let todo: Vec<u64> = app.items.iter().filter(|i| i.report.is_some() && !i.busy && !matches!(i.result, Some(Ok(_)))).map(|i| i.id).collect();
                let all = egui::Button::new(RichText::new(format!("clean all ({})", todo.len())).strong().color(Color32::WHITE)).fill(ACCENT);
                if ui.add_enabled(!todo.is_empty(), all).clicked() {
                    app.clean(ctx, todo);
                }
                let sel = app.selected.filter(|id| app.items.iter().any(|i| i.id == *id && i.report.is_some() && !i.busy));
                if ui.add_enabled(sel.is_some(), egui::Button::new("clean selected")).clicked() {
                    app.clean(ctx, sel.into_iter().collect());
                }
                let busy = app.items.iter().filter(|i| i.busy).count();
                if busy > 0 {
                    ui.spinner();
                    ui.label(RichText::new(format!("cleaning, {busy} to go")).color(DIM));
                }
            });
        });
        ui.add_space(10.0);
    });
}

fn status(item: &Item) -> (String, Color32) {
    if item.busy {
        return ("cleaning...".into(), ACCENT);
    }
    match (&item.result, &item.report) {
        (Some(Ok(c)), _) => {
            let left = c.after.count(Risk::High);
            if left > 0 {
                (format!("cleaned, but {left} sensitive field(s) are still in there"), AMBER)
            } else {
                (format!("cleaned, {} field(s) removed", c.removed), GREEN)
            }
        }
        (Some(Err(e)), _) => (e.clone(), RED),
        (None, None) => ("scanning...".into(), DIM),
        (None, Some(r)) => {
            let (hi, lo, all) = (r.count(Risk::High), r.count(Risk::Low), r.fields.len());
            let kind = r.kind.label();
            if hi > 0 {
                (format!("{kind} · {hi} sensitive, {all} fields"), RED)
            } else if lo > 0 {
                (format!("{kind} · {lo} worth a look, {all} fields"), AMBER)
            } else if all == 0 {
                (format!("{kind} · nothing found"), GREEN)
            } else {
                (format!("{kind} · {all} fields, nothing sensitive"), DIM)
            }
        }
    }
}

fn file_list(app: &mut Odin, ui: &mut egui::Ui) {
    egui::Panel::left("files").resizable(true).default_size(320.0).min_size(250.0).show(ui, |ui| {
        ui.add_space(10.0);
        let leaky = app.items.iter().filter(|i| i.report.as_ref().is_some_and(|r| r.count(Risk::High) > 0)).count();
        ui.label(RichText::new(format!("{} file(s), {leaky} with sensitive stuff", app.items.len())).small().color(DIM));
        ui.add_space(4.0);
        let mut clicked = None;
        egui::ScrollArea::vertical().auto_shrink([false, false]).show(ui, |ui| {
            for item in &app.items {
                let selected = app.selected == Some(item.id);
                let (text, color) = status(item);
                let fill = if selected { Color32::from_rgb(42, 38, 78) } else { Color32::TRANSPARENT };
                let row = egui::Frame::new().fill(fill).corner_radius(egui::CornerRadius::same(6)).inner_margin(egui::Margin::symmetric(8, 6)).show(ui, |ui| {
                    ui.set_width(ui.available_width());
                    ui.add(egui::Label::new(RichText::new(&item.name).strong()).truncate().selectable(false));
                    ui.add(egui::Label::new(RichText::new(text).small().color(color)).truncate().selectable(false));
                });
                if row.response.interact(egui::Sense::click()).clicked() {
                    clicked = Some(item.id);
                }
            }
        });
        if clicked.is_some() {
            app.selected = clicked;
            app.show_cleaned = false;
        }
    });
}

fn size_str(n: u64) -> String {
    match n {
        0..=1023 => format!("{n} bytes"),
        1024..=1_048_575 => format!("{:.1} kb", n as f64 / 1024.0),
        1_048_576..=1_073_741_823 => format!("{:.1} mb", n as f64 / 1_048_576.0),
        _ => format!("{:.2} gb", n as f64 / 1_073_741_824.0),
    }
}

fn open_folder(dir: &Path) {
    let tool = if cfg!(windows) {
        "explorer"
    } else if cfg!(target_os = "macos") {
        "open"
    } else {
        "xdg-open"
    };
    let _ = std::process::Command::new(tool).arg(dir).spawn();
}

fn empty(ui: &mut egui::Ui) {
    let rect = ui.available_rect_before_wrap().shrink(6.0);
    ui.painter().rect_stroke(rect, egui::CornerRadius::same(12), egui::Stroke::new(1.5, LINE), egui::StrokeKind::Inside);
    ui.vertical_centered(|ui| {
        ui.add_space(ui.available_height() * 0.32);
        ui.label(RichText::new("drop files or a folder here").size(26.0).strong());
        ui.add_space(4.0);
        ui.label(RichText::new("photos, videos, audio, pdfs, word / excel / powerpoint").color(DIM));
        ui.add_space(18.0);
        ui.label(RichText::new("nothing leaves this computer. originals are never changed,").small().color(DIM));
        ui.label(RichText::new("cleaned copies get saved next to them").small().color(DIM));
    });
}

fn viewer(app: &mut Odin, ui: &mut egui::Ui) {
    let Some(item) = app.selected.and_then(|id| app.items.iter().find(|i| i.id == id)) else {
        return empty(ui);
    };
    let mut show_cleaned = app.show_cleaned;

    ui.label(RichText::new(&item.name).size(20.0).strong());
    ui.add(egui::Label::new(RichText::new(item.path.display().to_string()).small().color(DIM)).truncate());
    ui.add_space(6.0);

    let cleaned = match &item.result {
        Some(Ok(c)) => Some(c),
        _ => None,
    };
    if let Some(Err(e)) = &item.result {
        ui.label(RichText::new(format!("couldn't clean this one: {e}")).color(RED));
    }
    if let Some(c) = cleaned {
        ui.horizontal(|ui| {
            ui.selectable_value(&mut show_cleaned, false, "original");
            ui.selectable_value(&mut show_cleaned, true, "cleaned copy");
            ui.add_space(8.0);
            let out_name = c.out.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
            ui.label(RichText::new(format!("removed {} field(s), saved as {out_name}", c.removed)).color(GREEN));
            if ui.button("show in folder").clicked() {
                open_folder(c.out.parent().unwrap_or(&c.out));
            }
        });
    }
    let report = match (cleaned, show_cleaned) {
        (Some(c), true) => Some(&c.after),
        _ => item.report.as_ref(),
    };
    ui.add_space(4.0);
    match report {
        None => {
            ui.horizontal(|ui| {
                ui.spinner();
                ui.label("scanning...");
            });
        }
        Some(r) => {
            egui::ScrollArea::vertical().auto_shrink([false, false]).show(ui, |ui| report_view(ui, r));
        }
    }
    app.show_cleaned = show_cleaned;
}

fn report_view(ui: &mut egui::Ui, r: &Report) {
    let (hi, lo) = (r.count(Risk::High), r.count(Risk::Low));
    ui.horizontal(|ui| {
        ui.label(RichText::new(format!("{} · {} · {} fields", r.kind.label(), size_str(r.size), r.fields.len())).color(DIM));
        if hi > 0 {
            ui.label(RichText::new(format!("{hi} sensitive")).color(RED).strong());
        }
        if lo > 0 {
            ui.label(RichText::new(format!("{lo} worth a look")).color(AMBER));
        }
    });
    ui.add_space(6.0);

    if r.fields.is_empty() && r.notes.is_empty() {
        ui.label(RichText::new("nothing found in this file").size(16.0).color(GREEN));
        return;
    }
    if !r.findings.is_empty() {
        egui::Frame::new().fill(CARD).corner_radius(egui::CornerRadius::same(8)).inner_margin(egui::Margin::same(12)).show(ui, |ui| {
            ui.set_width(ui.available_width());
            ui.label(RichText::new("what stands out").strong());
            ui.add_space(2.0);
            for f in &r.findings {
                ui.horizontal_wrapped(|ui| {
                    ui.label(RichText::new(f.kind).color(risk_color(f.risk)).strong());
                    ui.label(&f.detail);
                });
            }
        });
        ui.add_space(8.0);
    }
    let value_width = (ui.available_width() - 420.0).max(220.0);
    for g in Group::ALL {
        let fields: Vec<_> = r.group(g).collect();
        if fields.is_empty() {
            continue;
        }
        let worst = fields.iter().map(|f| f.risk).max().unwrap_or(Risk::None);
        let title = RichText::new(format!("{}  ({})", g.label(), fields.len())).strong().color(risk_color(worst));
        egui::CollapsingHeader::new(title).id_salt(g.label()).default_open(g != Group::Other).show(ui, |ui| {
            egui::Grid::new(g.label()).num_columns(3).striped(true).spacing([14.0, 6.0]).min_col_width(150.0).max_col_width(value_width).show(ui, |ui| {
                for f in fields {
                    let key = RichText::new(&f.key).color(risk_color(f.risk));
                    let hover = match f.risk {
                        Risk::High => "sensitive: can point at a person, a place or a device",
                        Risk::Low => "worth a look: dates, device models, edit trails",
                        Risk::None => "harmless on its own",
                    };
                    ui.label(if f.risk == Risk::High { key.strong() } else { key }).on_hover_text(hover);
                    ui.add(egui::Label::new(&f.value).wrap());
                    ui.label(RichText::new(&f.source).small().color(DIM));
                    ui.end_row();
                }
            });
        });
    }
    for n in &r.notes {
        ui.label(RichText::new(n).small().color(AMBER));
    }
}

// dim the window while something is being dragged over it
fn drop_overlay(ctx: &egui::Context) {
    if ctx.input(|i| i.raw.hovered_files.is_empty()) {
        return;
    }
    let rect = ctx.content_rect();
    let painter = ctx.layer_painter(egui::LayerId::new(egui::Order::Foreground, egui::Id::unique("drop")));
    painter.rect_filled(rect, 0.0, Color32::from_black_alpha(190));
    painter.text(rect.center(), egui::Align2::CENTER_CENTER, "drop to add", egui::FontId::proportional(30.0), Color32::WHITE);
}
