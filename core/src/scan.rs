// turns whatever got dropped on the window (files, folders, a mix) into a flat file list

use std::path::{Path, PathBuf};

pub const OUT_DIR: &str = "odin-clean";

pub fn collect(paths: &[PathBuf]) -> Vec<PathBuf> {
    let mut out = vec![];
    for p in paths {
        walk(p, &mut out, 0);
    }
    out.sort();
    out.dedup();
    out
}

fn walk(p: &Path, out: &mut Vec<PathBuf>, depth: usize) {
    // symlinks are skipped so a link loop can't trap us
    let Ok(meta) = std::fs::symlink_metadata(p) else { return };
    if meta.is_file() {
        out.push(p.to_path_buf());
    } else if meta.is_dir() && depth < 32 {
        let Ok(rd) = std::fs::read_dir(p) else { return };
        for e in rd.flatten() {
            let name = e.file_name();
            let name = name.to_string_lossy();
            // dotfiles + our own output folder stay out of it
            if name.starts_with('.') || name == OUT_DIR {
                continue;
            }
            walk(&e.path(), out, depth + 1);
        }
    }
}
