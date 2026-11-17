// c2pa / content credentials ride in jumbf boxes. we don't verify signatures or
// decode the cbor, just notice it's there and fish out what it says about the maker

use crate::ai;
use crate::report::{Group, Report, Risk};

pub const C2PA_UUID: [u8; 16] = [0xd8, 0xfe, 0xc3, 0xd6, 0x1b, 0x0e, 0x48, 0x3c, 0x92, 0x97, 0x58, 0x28, 0x87, 0x7e, 0xc4, 0x81];

pub fn looks_like_c2pa(b: &[u8]) -> bool {
    let has = |n: &[u8]| b.windows(n.len()).any(|w| w == n);
    has(b"c2pa") || has(b"jumb") && has(b"c2ma")
}

pub fn describe(rep: &mut Report, b: &[u8], place: &str) {
    const SRC: &str = "C2PA";
    rep.c2pa = true;
    rep.add_as(Group::Ai, Risk::High, SRC, "Content Credentials", &format!("manifest in {place} ({} bytes)", b.len()));
    let text = String::from_utf8_lossy(b);
    if text.contains("trainedAlgorithmicMedia") || text.contains("TrainedAlgorithmicMedia") {
        rep.add_as(Group::Ai, Risk::High, SRC, "Digital source type", "says this was made by an ai model");
    }
    if let Some(g) = ai::generator_in(&text) {
        rep.add_as(Group::Ai, Risk::High, SRC, "Generator", &format!("mentions {g}"));
    }
    for label in ["c2pa.actions", "c2pa.ingredient", "stds.schema-org.CreativeWork", "c2pa.training-mining", "cawg.identity"] {
        if text.contains(label) {
            rep.add_as(Group::Ai, Risk::Low, SRC, "Assertion", label);
        }
    }
}
