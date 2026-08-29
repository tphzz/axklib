import type { WaveformBin } from './types';

export interface CanvasPixelSize {
    width: number;
    height: number;
}

export interface WaveformPixelColumn {
    x: number;
    y: number;
    width: number;
    height: number;
}

interface DevicePixelRatioSource {
    devicePixelRatio: number;
    matchMedia?(query: string): MediaQueryList;
}

function positiveInteger(value: number): number {
    return Math.max(1, Math.round(Number.isFinite(value) ? value : 1));
}

export function canvasPixelSize(
    cssWidth: number,
    cssHeight: number,
    devicePixelRatio: number,
    observedDeviceSize?: CanvasPixelSize | null,
): CanvasPixelSize {
    if (observedDeviceSize) {
        return {
            width: positiveInteger(observedDeviceSize.width),
            height: positiveInteger(observedDeviceSize.height),
        };
    }
    const scale = Math.max(1, Number.isFinite(devicePixelRatio) ? devicePixelRatio : 1);
    return {
        width: positiveInteger(cssWidth * scale),
        height: positiveInteger(cssHeight * scale),
    };
}

export function resizeObserverDevicePixelSize(entry: ResizeObserverEntry): CanvasPixelSize | null {
    const rawSize = entry.devicePixelContentBoxSize as ResizeObserverSize | readonly ResizeObserverSize[] | undefined;
    const size = Array.isArray(rawSize) ? rawSize[0] : rawSize;
    if (!size || !Number.isFinite(size.inlineSize) || !Number.isFinite(size.blockSize)) return null;
    return {
        width: positiveInteger(size.inlineSize),
        height: positiveInteger(size.blockSize),
    };
}

export function waveformPixelColumns(
    values: readonly WaveformBin[],
    contentRatio: number,
    cssWidth: number,
    pixelWidth: number,
    pixelHeight: number,
): WaveformPixelColumn[] {
    if (values.length === 0 || contentRatio <= 0 || cssWidth <= 0 || pixelWidth <= 0 || pixelHeight <= 0) return [];
    const normalizedRatio = Math.min(1, contentRatio);
    const contentPixelWidth = Math.max(1, Math.min(pixelWidth, Math.round(pixelWidth * normalizedRatio)));
    const columnCount = Math.max(1, Math.min(contentPixelWidth, Math.round(cssWidth * normalizedRatio)));
    let peak = 1;
    for (const value of values) peak = Math.max(peak, Math.abs(value.minimum), Math.abs(value.maximum));
    const center = pixelHeight / 2;
    const columns: WaveformPixelColumn[] = [];
    for (let column = 0; column < columnCount; column += 1) {
        const first = Math.floor((column / columnCount) * values.length);
        const last = Math.max(first + 1, Math.ceil(((column + 1) / columnCount) * values.length));
        let minimum = 0;
        let maximum = 0;
        for (let index = first; index < Math.min(last, values.length); index += 1) {
            minimum = Math.min(minimum, values[index]?.minimum ?? 0);
            maximum = Math.max(maximum, values[index]?.maximum ?? 0);
        }
        const x = Math.round((column / columnCount) * contentPixelWidth);
        const right = Math.round(((column + 1) / columnCount) * contentPixelWidth);
        const y = Math.max(0, Math.min(pixelHeight - 1, Math.round(center - (maximum / peak) * center)));
        const bottom = Math.max(y + 1, Math.min(pixelHeight, Math.round(center - (minimum / peak) * center)));
        columns.push({ x, y, width: Math.max(1, right - x), height: bottom - y });
    }
    return columns;
}

export function observeDevicePixelRatio(onChange: () => void, source: DevicePixelRatioSource = window): () => void {
    if (typeof source.matchMedia !== 'function') return () => undefined;
    let media: MediaQueryList | null = null;
    const handleChange = (): void => {
        subscribe();
        onChange();
    };
    const subscribe = (): void => {
        media?.removeEventListener('change', handleChange);
        const ratio =
            Number.isFinite(source.devicePixelRatio) && source.devicePixelRatio > 0 ? source.devicePixelRatio : 1;
        media = source.matchMedia!(`(resolution: ${ratio}dppx)`);
        media.addEventListener('change', handleChange);
    };
    subscribe();
    return () => media?.removeEventListener('change', handleChange);
}
