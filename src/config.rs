//! Layer configuration, read from environment variables.
//!
//! | Variable           | Meaning                                                    |
//! | ------------------ | ---------------------------------------------------------- |
//! | `MIRROR_MODE`      | `depth`/`on`/`1` (default) or `off`/`none`/`0`.            |
//! | `MIRROR_HIGHLIGHT` | `<hash>=<RRGGBB>[,<hash>=<RRGGBB>...]` materials to color. |
//! | `MIRROR_LOG`       | any non-empty value other than `0` enables stderr logging. |
//!
//! `<hash>` is the 64-bit FNV-1a hash (hex) of a fragment shader's SPIR-V and
//! `<RRGGBB>` is an sRGB hex color. Loading never fails: invalid values fall
//! back to defaults with a diagnostic on stderr.

/// Depth visualization or passthrough.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Mode {
    /// Materials pass straight through to the driver.
    Off,
    /// Fragment shaders are replaced with the depth/highlight visualization.
    #[default]
    Depth,
}

impl Mode {
    /// Parses a `MIRROR_MODE` value, returning `None` for anything unrecognized.
    #[must_use]
    pub fn parse(text: &str) -> Option<Self> {
        match text {
            "off" | "0" | "none" => Some(Self::Off),
            "depth" | "on" | "1" => Some(Self::Depth),
            _ => None,
        }
    }
}

/// A material (by shader hash) painted in a fixed color.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Highlight {
    pub hash: u64,
    /// Linear RGB in `[0, 1]`, each channel decoded from a hex byte.
    pub color: [f32; 3],
}

/// Fully resolved layer configuration.
#[derive(Clone, Debug, Default)]
pub struct Config {
    pub mode: Mode,
    pub log: bool,
    pub highlights: Vec<Highlight>,
}

impl Config {
    /// Reads the configuration from the environment, never failing.
    #[must_use]
    pub fn from_env() -> Self {
        let mut config = Self::default();

        if let Ok(mode) = std::env::var("MIRROR_MODE") {
            match Mode::parse(&mode) {
                Some(parsed) => config.mode = parsed,
                None => eprintln!("[mirror] unknown MIRROR_MODE '{mode}', defaulting to 'depth'"),
            }
        }

        config.log =
            matches!(std::env::var("MIRROR_LOG"), Ok(value) if !value.is_empty() && value != "0");

        if let Ok(highlight) = std::env::var("MIRROR_HIGHLIGHT") {
            if !highlight.is_empty() && !config.set_highlights(&highlight) {
                eprintln!(
                    "[mirror] invalid MIRROR_HIGHLIGHT '{highlight}', expected \
                     <hash>=<RRGGBB>[,<hash>=<RRGGBB>...]"
                );
            }
        }

        config
    }

    /// Replaces the highlight list from a `MIRROR_HIGHLIGHT` string. On any parse
    /// error the existing highlights are left untouched and `false` is returned.
    pub fn set_highlights(&mut self, text: &str) -> bool {
        match parse_highlights(text) {
            Some(highlights) => {
                self.highlights = highlights;
                true
            }
            None => false,
        }
    }

    /// Returns the highlight registered for `hash`, if any.
    #[must_use]
    pub fn highlight(&self, hash: u64) -> Option<&Highlight> {
        self.highlights.iter().find(|entry| entry.hash == hash)
    }
}

/// Parses an `RRGGBB` hex color into linear `[0, 1]` channels.
#[must_use]
pub fn parse_color(text: &str) -> Option<[f32; 3]> {
    let bytes = text.as_bytes();
    if bytes.len() != 6 {
        return None;
    }
    let mut color = [0.0_f32; 3];
    for (channel, slot) in color.iter_mut().enumerate() {
        let high = hex_digit(bytes[channel * 2])?;
        let low = hex_digit(bytes[channel * 2 + 1])?;
        *slot = f32::from(high * 16 + low) / 255.0;
    }
    Some(color)
}

/// Parses a full `MIRROR_HIGHLIGHT` list. All-or-nothing: a single malformed
/// entry rejects the whole string.
#[must_use]
pub fn parse_highlights(text: &str) -> Option<Vec<Highlight>> {
    text.split(',').map(parse_highlight_entry).collect()
}

fn parse_highlight_entry(entry: &str) -> Option<Highlight> {
    let (hash_text, color_text) = entry.split_once('=')?;
    if hash_text.is_empty() || hash_text.len() > 16 {
        return None;
    }
    let hash = hash_text.bytes().try_fold(0_u64, |hash, byte| {
        Some(hash << 4 | u64::from(hex_digit(byte)?))
    })?;
    Some(Highlight {
        hash,
        color: parse_color(color_text)?,
    })
}

fn hex_digit(byte: u8) -> Option<u8> {
    match byte {
        b'0'..=b'9' => Some(byte - b'0'),
        b'a'..=b'f' => Some(byte - b'a' + 10),
        b'A'..=b'F' => Some(byte - b'A' + 10),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn near(a: f32, b: f32) -> bool {
        (a - b).abs() < 1e-6
    }

    #[test]
    fn parse_mode() {
        assert_eq!(Mode::parse("off"), Some(Mode::Off));
        assert_eq!(Mode::parse("0"), Some(Mode::Off));
        assert_eq!(Mode::parse("none"), Some(Mode::Off));
        assert_eq!(Mode::parse("depth"), Some(Mode::Depth));
        assert_eq!(Mode::parse("on"), Some(Mode::Depth));
        assert_eq!(Mode::parse("1"), Some(Mode::Depth));
        assert_eq!(Mode::parse("banana"), None);
        assert_eq!(Mode::parse(""), None);
    }

    #[test]
    fn parse_color_channels() {
        let red = parse_color("ff0000").unwrap();
        assert!(near(red[0], 1.0) && near(red[1], 0.0) && near(red[2], 0.0));
        assert!(near(parse_color("00FF00").unwrap()[1], 1.0));
        assert!(near(parse_color("000080").unwrap()[2], 128.0 / 255.0));
        assert_eq!(parse_color("ff000"), None);
        assert_eq!(parse_color("gg0000"), None);
        assert_eq!(parse_color("ff00001"), None);
    }

    #[test]
    fn parse_highlight_list() {
        let mut config = Config::default();

        assert!(config.set_highlights("deadbeefcafef00d=00ff00"));
        assert_eq!(config.highlights.len(), 1);
        assert_eq!(config.highlights[0].hash, 0xdead_beef_cafe_f00d);
        assert!(near(config.highlights[0].color[1], 1.0));

        assert!(config.set_highlights("1=ff0000,2=00ff00,3=0000ff"));
        assert_eq!(config.highlights.len(), 3);
        assert_eq!(config.highlights[2].hash, 3);
        assert!(config.highlight(2).is_some());
        assert!(config.highlight(4).is_none());

        // Invalid input must not clobber existing highlights.
        assert!(!config.set_highlights("not-a-highlight"));
        assert!(!config.set_highlights("toolonghash00000000=ff0000"));
        assert!(!config.set_highlights("12=zzz"));
        assert!(!config.set_highlights("=ff0000"));
        assert!(!config.set_highlights("12=ff0000,"));
        assert_eq!(config.highlights.len(), 3);
    }

    #[test]
    fn load_from_env() {
        // This is the only test that touches the process environment.
        let clear = || {
            std::env::remove_var("MIRROR_MODE");
            std::env::remove_var("MIRROR_HIGHLIGHT");
            std::env::remove_var("MIRROR_LOG");
        };

        clear();
        let config = Config::from_env();
        assert_eq!(config.mode, Mode::Depth);
        assert!(!config.log);
        assert!(config.highlights.is_empty());

        std::env::set_var("MIRROR_MODE", "off");
        std::env::set_var("MIRROR_LOG", "1");
        std::env::set_var("MIRROR_HIGHLIGHT", "abc=123456");
        let config = Config::from_env();
        assert_eq!(config.mode, Mode::Off);
        assert!(config.log);
        assert_eq!(config.highlights.len(), 1);
        assert_eq!(config.highlights[0].hash, 0xabc);

        // Garbage values fall back to defaults instead of failing.
        std::env::set_var("MIRROR_MODE", "garbage");
        std::env::set_var("MIRROR_LOG", "0");
        std::env::set_var("MIRROR_HIGHLIGHT", "garbage");
        let config = Config::from_env();
        assert_eq!(config.mode, Mode::Depth);
        assert!(!config.log);
        assert!(config.highlights.is_empty());

        clear();
    }
}
