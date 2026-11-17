// one file per format. each has a read() that fills a Report and, where we can
// write the format back out, a clean() that returns the stripped bytes

pub mod exif;
pub mod gif;
pub mod icc;
pub mod iptc;
pub mod isobmff;
pub mod jpeg;
pub mod jumbf;
pub mod media;
pub mod office;
pub mod pdf;
pub mod png;
pub mod webp;
pub mod xmp;

pub(crate) fn be16(b: &[u8], i: usize) -> Option<u16> {
    Some(u16::from_be_bytes(b.get(i..i + 2)?.try_into().ok()?))
}
pub(crate) fn be32(b: &[u8], i: usize) -> Option<u32> {
    Some(u32::from_be_bytes(b.get(i..i + 4)?.try_into().ok()?))
}
pub(crate) fn be64(b: &[u8], i: usize) -> Option<u64> {
    Some(u64::from_be_bytes(b.get(i..i + 8)?.try_into().ok()?))
}
pub(crate) fn le32(b: &[u8], i: usize) -> Option<u32> {
    Some(u32::from_le_bytes(b.get(i..i + 4)?.try_into().ok()?))
}
