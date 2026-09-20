export function endUnitFrames(rate: number, tempo: number, endType: number): number | undefined {
    if (endType === 0 || endType === 1) return 1;
    if (!Number.isFinite(rate) || rate <= 0) return undefined;
    if (endType === 2) return rate;
    if (endType === 3 && Number.isFinite(tempo) && tempo > 0) return (rate * 6000) / tempo;
    return undefined;
}

export function endDisplay(frame: number, start: number, rate: number, tempo: number, endType: number) {
    const scale = endUnitFrames(rate, tempo, endType);
    return scale === undefined ? undefined : (frame - (endType === 0 ? 0 : start)) / scale;
}

export function endFrame(value: number, start: number, rate: number, tempo: number, endType: number) {
    const scale = endUnitFrames(rate, tempo, endType);
    return scale === undefined ? undefined : Math.round(value * scale + (endType === 0 ? 0 : start));
}

export function calculatedLoopTempo(rate: number, frames: number, beats = 4, normalize = true) {
    if (![rate, frames, beats].every((value) => Number.isFinite(value) && value > 0)) return undefined;
    let tempo = (6000 * rate * beats) / frames;
    if (!Number.isFinite(tempo) || tempo <= 0) return undefined;
    if (normalize) {
        // Yamaha's Calculate operation octave-normalizes a four-beat interpretation.
        while (tempo < 8000) tempo *= 2;
        while (tempo >= 16000) tempo /= 2;
    }
    const rounded = Math.round(tempo);
    return rounded >= 8000 && rounded <= 15999 ? rounded : undefined;
}
