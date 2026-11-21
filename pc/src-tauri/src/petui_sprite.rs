//! Encode PNG artwork for the firmware WASM sprite decoder.
use image::ImageDecoder;
use std::{collections::BTreeMap, fs, io::BufReader, path::Path};

type ColorCounts = Vec<(u16, u32)>;

pub(super) struct Sprite {
    pub width: u32,
    pub height: u32,
    pub bytes: Vec<u8>,
}

fn rgb565(r: u8, g: u8, b: u8) -> u16 {
    (u16::from(r >> 3) << 11) | (u16::from(g >> 2) << 5) | u16::from(b >> 3)
}

fn channels(color: u16) -> [u16; 3] {
    [
        (color >> 11) << 3,
        ((color >> 5) & 63) << 2,
        (color & 31) << 3,
    ]
}

fn split_score(colors: &[(u16, u32)]) -> (u32, usize) {
    let mut min = [u16::MAX; 3];
    let mut max = [0; 3];
    let mut population = 0;
    for &(color, count) in colors {
        for (axis, value) in channels(color).into_iter().enumerate() {
            min[axis] = min[axis].min(value);
            max[axis] = max[axis].max(value);
        }
        population += count;
    }
    (0..3)
        .map(|axis| (u32::from(max[axis] - min[axis]) * population, axis))
        .max()
        .unwrap_or_default()
}

fn palette(histogram: BTreeMap<u16, u32>) -> (Vec<u16>, BTreeMap<u16, u8>) {
    const MAX_COLORS: usize = 254;
    let mut boxes: Vec<ColorCounts> = if histogram.len() <= MAX_COLORS {
        histogram.into_iter().map(|entry| vec![entry]).collect()
    } else {
        let mut boxes = vec![histogram.into_iter().collect::<ColorCounts>()];
        while boxes.len() < MAX_COLORS {
            let (_, axis, index) = boxes
                .iter()
                .enumerate()
                .map(|(i, colors)| {
                    let (score, axis) = split_score(colors);
                    (score, axis, i)
                })
                .max()
                .unwrap();
            let mut colors = boxes.swap_remove(index);
            colors.sort_by_key(|&(color, _)| channels(color)[axis]);
            let total: u32 = colors.iter().map(|&(_, count)| count).sum();
            let mut seen = 0;
            let split = colors
                .iter()
                .take(colors.len() - 1)
                .take_while(|&&(_, count)| {
                    seen += count;
                    seen * 2 < total
                })
                .count()
                .min(colors.len() - 2)
                + 1;
            let upper = colors.split_off(split);
            boxes.extend([colors, upper]);
        }
        boxes
    };

    let mut palette = Vec::with_capacity(boxes.len());
    let mut indices = BTreeMap::new();
    for (index, colors) in boxes.drain(..).enumerate() {
        let mut weighted = [0; 3];
        let mut total = 0;
        for (color, count) in colors {
            total += count;
            for (axis, value) in channels(color).into_iter().enumerate() {
                weighted[axis] += u32::from(value) * count;
            }
            // Zero repeats the previous row and one is transparent.
            indices.insert(color, (index + 2) as u8);
        }
        let [r, g, b] = weighted.map(|sum| (sum / total) as u8);
        palette.push(rgb565(r, g, b));
    }
    if palette.is_empty() {
        palette.push(0);
    }
    (palette, indices)
}

fn encode_runs(values: &[u8], out: &mut Vec<u8>) {
    let mut at = 0;
    while at < values.len() {
        let start = at;
        while at + 1 < values.len() && at - start < 127 && values[at + 1] == values[start] {
            at += 1;
        }
        if at > start {
            out.extend([128 | (at - start) as u8, values[start]]);
            at += 1;
            continue;
        }
        at += 1;
        while at < values.len() && at - start < 128 {
            if at + 1 < values.len() && values[at] == values[at + 1] {
                break;
            }
            at += 1;
        }
        out.push((at - start - 1) as u8);
        out.extend_from_slice(&values[start..at]);
    }
}

pub(super) fn encode(path: &Path) -> Result<Sprite, String> {
    let file = fs::File::open(path).map_err(|error| error.to_string())?;
    if file.metadata().map_err(|e| e.to_string())?.len() > 8 * 1024 * 1024 {
        return Err("精灵 PNG 源文件超过 8 MiB".into());
    }
    let decoder = image::codecs::png::PngDecoder::new(BufReader::new(file))
        .map_err(|e| format!("{}：{e}", path.display()))?;
    let (width, height) = decoder.dimensions();
    if !(1..=640).contains(&width) || !(1..=480).contains(&height) {
        return Err("精灵尺寸超出 640×480".into());
    }
    let image = image::DynamicImage::from_decoder(decoder)
        .map_err(|e| e.to_string())?
        .into_rgba8();
    let mut histogram = BTreeMap::new();
    let pixels: Vec<_> = image
        .pixels()
        .map(|pixel| {
            let [red, green, blue, alpha] = pixel.0;
            if alpha < 128 {
                return None;
            }
            let color = rgb565(red, green, blue);
            *histogram.entry(color).or_insert(0) += 1;
            Some(color)
        })
        .collect();
    let (palette, indices) = palette(histogram);
    let mut previous = vec![1; width as usize];
    let values: Vec<_> = pixels
        .into_iter()
        .enumerate()
        .map(|(i, color)| {
            let index = color.map_or(1, |color| indices[&color]);
            let prior = std::mem::replace(&mut previous[i % width as usize], index);
            if prior == index {
                0
            } else {
                index
            }
        })
        .collect();
    let mut bytes = vec![palette.len() as u8];
    for color in palette {
        bytes.extend(color.to_le_bytes());
    }
    encode_runs(&values, &mut bytes);
    if bytes.len() > 64 * 1024 {
        return Err(format!(
            "{} 的精灵编码超过 64 KiB，请将图片切片",
            path.display()
        ));
    }
    Ok(Sprite {
        width,
        height,
        bytes,
    })
}
