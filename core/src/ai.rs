// spots "an ai made this" tags. only looks at metadata, it can't see invisible
// watermarks baked into the pixels (synthid and friends), nothing here touches those

use crate::classify::local_key;

// names that show up in software / credit style fields
const GENERATORS: &[&str] = &[
    "dall-e", "dall·e", "dalle", "midjourney", "stable diffusion", "stablediffusion", "stable-diffusion", "sdxl",
    "adobe firefly", "firefly", "google imagen", "imagen", "made with google ai", "gemini", "chatgpt", "openai",
    "sora", "novelai", "comfyui", "automatic1111", "invokeai", "invoke ai", "leonardo.ai", "ideogram",
    "bing image creator", "image creator from designer", "microsoft designer", "grok", "runway", "dreamstudio",
    "stability ai", "playground ai", "recraft", "nightcafe", "imagine with meta", "meta ai", "ai generated",
    "ai-generated", "black forest labs", "flux.1", "fooocus", "draw things", "krea", "freepik ai", "kling",
    "luma dream", "pika labs", "hailuo", "seedream", "nano banana", "trainedalgorithmicmedia",
];
const SOFTWARE_KEYS: &[&str] = &[
    "software", "creatortool", "producer", "creator", "make", "model", "credit", "source", "softwareagent",
    "generator", "claim_generator", "application", "encoder", "tool", "originatingprogram", "by-line", "artist",
];
// png text keys the usual generators write
const PNG_KEYS: &[&str] = &[
    "parameters", "prompt", "workflow", "invokeai_metadata", "invokeai_graph", "sd-metadata", "dream",
    "generation_data", "negative_prompt", "aesthetic_score", "generation time",
];

pub fn generator_in(text: &str) -> Option<&'static str> {
    let t = text.to_ascii_lowercase();
    GENERATORS.iter().copied().find(|g| t.contains(g))
}

// Some(why) when a field looks like an ai-generation tag
pub fn hint(source: &str, key: &str, value: &str) -> Option<String> {
    let k = local_key(key);
    let v = value.to_ascii_lowercase();
    if k == "digitalsourcetype" && (v.contains("algorithmicmedia") || v.contains("compositesynthetic")) {
        return Some("marked as ai generated (digital source type)".into());
    }
    if PNG_KEYS.contains(&k.as_str()) && source == "PNG text" {
        return Some("generation settings / prompt".into());
    }
    // stable diffusion webui dumps its settings in a comment field
    if (v.contains("steps: ") && v.contains("sampler: ")) || v.contains("negative prompt: ") {
        return Some("stable diffusion settings".into());
    }
    if v.starts_with('{') && v.contains("\"sampler\"") && v.contains("\"steps\"") {
        return Some("generation settings".into());
    }
    if k.contains("aiprompt") || k.contains("aisystem") || k.starts_with("ai generated") {
        return Some("ai info field".into());
    }
    if SOFTWARE_KEYS.contains(&k.as_str()) {
        if let Some(g) = generator_in(&v) {
            return Some(format!("mentions {g}"));
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hints() {
        assert!(hint("XMP", "Iptc4xmpExt:DigitalSourceType", "http://cv.iptc.org/newscodes/digitalsourcetype/trainedAlgorithmicMedia").is_some());
        assert!(hint("PNG text", "parameters", "a cat\nSteps: 20, Sampler: Euler a").is_some());
        assert!(hint("EXIF", "Software", "Adobe Firefly 3.0").is_some());
        assert!(hint("EXIF", "UserComment", "masterpiece\nNegative prompt: blurry\nSteps: 30, Sampler: DPM++").is_some());
        assert!(hint("EXIF", "Software", "Adobe Photoshop 25.0").is_none());
        assert!(hint("EXIF", "ImageDescription", "una imagen de mi gato").is_none());
    }
}
