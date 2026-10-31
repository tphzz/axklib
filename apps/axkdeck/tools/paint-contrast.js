function luminance(color) {
    return color.map(channel => {
        const value = channel / 255;
        return value <= 0.04045 ? value / 12.92 : ((value + 0.055) / 1.055) ** 2.4;
    }).reduce((sum, value, index) => sum + value * [0.2126, 0.7152, 0.0722][index], 0);
}

export function contrastRatio(foreground, background) {
    const a = luminance(foreground), b = luminance(background);
    return (Math.max(a, b) + 0.05) / (Math.min(a, b) + 0.05);
}

export function minimumGradientContrast(foreground, stops) {
    if (stops.length < 2) throw new RangeError('A gradient requires at least two color stops');
    let minimum = Infinity;
    for (let segment = 1; segment < stops.length; segment++) {
        const start = stops[segment - 1], end = stops[segment];
        for (let step = 0; step <= 1000; step++) {
            const color = start.map((value, channel) => value + (end[channel] - value) * step / 1000);
            minimum = Math.min(minimum, contrastRatio(foreground, color));
        }
    }
    return minimum;
}
