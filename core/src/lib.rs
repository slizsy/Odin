//! odin's engine. reads metadata out of files, groups it, and strips it.
//! no ui and no network in here, the app crate sits on top of this

pub mod ai;
pub mod classify;
pub mod clean;
pub mod formats;
pub mod inspect;
pub mod kind;
pub mod rename;
pub mod report;
pub mod scan;
pub mod xml;
pub mod zip;

pub use clean::{clean_file, CleanError, CleanOptions, Outcome};
pub use inspect::{inspect, inspect_bytes};
pub use kind::Kind;
pub use rename::RenameMode;
pub use report::{Field, Finding, Gps, Group, Report, Risk, Thumb};
