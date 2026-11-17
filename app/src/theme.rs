// colours + spacing in one place so the whole app changes together

use eframe::egui::{self, Color32};
use odin_core::Risk;

pub const BG: Color32 = Color32::from_rgb(14, 16, 20);
pub const PANEL: Color32 = Color32::from_rgb(20, 23, 29);
pub const CARD: Color32 = Color32::from_rgb(27, 31, 39);
pub const LINE: Color32 = Color32::from_rgb(44, 50, 62);
pub const TEXT: Color32 = Color32::from_rgb(222, 226, 235);
pub const DIM: Color32 = Color32::from_rgb(132, 141, 158);
pub const ACCENT: Color32 = Color32::from_rgb(132, 110, 255);
pub const RED: Color32 = Color32::from_rgb(255, 104, 112);
pub const AMBER: Color32 = Color32::from_rgb(240, 184, 80);
pub const GREEN: Color32 = Color32::from_rgb(88, 208, 142);

pub fn risk_color(r: Risk) -> Color32 {
    match r {
        Risk::High => RED,
        Risk::Low => AMBER,
        Risk::None => TEXT,
    }
}

pub fn apply(ctx: &egui::Context) {
    let mut v = egui::Visuals::dark();
    v.panel_fill = PANEL;
    v.window_fill = PANEL;
    v.extreme_bg_color = BG;
    v.faint_bg_color = Color32::from_rgb(24, 27, 34);
    v.override_text_color = Some(TEXT);
    v.hyperlink_color = ACCENT;
    v.selection.bg_fill = Color32::from_rgb(58, 50, 112);
    v.selection.stroke.color = TEXT;
    v.widgets.noninteractive.bg_stroke.color = LINE;
    v.widgets.inactive.weak_bg_fill = CARD;
    v.widgets.inactive.bg_fill = CARD;
    v.widgets.hovered.weak_bg_fill = Color32::from_rgb(38, 43, 54);
    v.widgets.active.weak_bg_fill = Color32::from_rgb(48, 54, 68);
    ctx.set_visuals(v);
    ctx.global_style_mut(|s| {
        s.spacing.item_spacing = egui::vec2(8.0, 6.0);
        s.spacing.button_padding = egui::vec2(10.0, 5.0);
        for font in s.text_styles.values_mut() {
            font.size *= 1.1;
        }
    });
}
