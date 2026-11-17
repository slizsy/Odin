// dev helper, not part of the app: prints what the engine sees in a file, and with
// --clean <dir> runs the cleaner and prints what's left
//   cargo run -p odin-core --example dump -- photo.jpg
//   cargo run -p odin-core --example dump -- --clean out/ photo.jpg video.mp4

use odin_core::{clean_file, inspect, CleanOptions, Group, RenameMode, Report, Risk};
use std::path::{Path, PathBuf};

fn show(r: &Report) {
    println!("  kind: {}   size: {}   fields: {}", r.kind.label(), r.size, r.fields.len());
    for g in Group::ALL {
        let fields: Vec<_> = r.group(g).collect();
        if fields.is_empty() {
            continue;
        }
        println!("  [{}]", g.label());
        for f in fields {
            let mark = match f.risk {
                Risk::High => "!!",
                Risk::Low => " !",
                Risk::None => "  ",
            };
            let v: String = f.value.chars().take(90).collect();
            println!("   {mark} {:<18} {:<34} {v}", f.source, f.key);
        }
    }
    for g in &r.gps {
        println!("  gps: {:.6}, {:.6} alt {:?} accuracy {:?}", g.lat, g.lon, g.alt, g.accuracy_m);
    }
    for f in &r.findings {
        println!("  finding ({:?}) {}: {}", f.risk, f.kind, f.detail);
    }
    for t in &r.thumbs {
        println!("  preview: {} ({} bytes)", t.label, t.data.len());
    }
    for n in &r.notes {
        println!("  note: {n}");
    }
}

fn main() {
    let mut args: Vec<String> = std::env::args().skip(1).collect();
    let out = (args.first().map(String::as_str) == Some("--clean")).then(|| {
        args.remove(0);
        PathBuf::from(args.remove(0))
    });
    for a in &args {
        println!("== {a}");
        match &out {
            None => show(&inspect(Path::new(a))),
            Some(dir) => match clean_file(Path::new(a), dir, RenameMode::Keep, &CleanOptions::default()) {
                Ok(o) => {
                    println!("  before: {} fields, {} sensitive", o.before.fields.len(), o.before.count(Risk::High));
                    println!("  -> {} (removed {})", o.out.display(), o.removed());
                    show(&o.after);
                }
                Err(e) => println!("  FAILED: {e}"),
            },
        }
    }
}
