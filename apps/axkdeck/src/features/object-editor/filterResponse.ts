export interface FilterStage {
    kind: 'lowpass' | 'highpass' | 'bandpass' | 'notch' | 'peak';
    cutoff: number;
    resonance: number;
    width: number;
    order?: number;
    mix?: number;
}

// An illustrative response family in relative coordinates, not DSP coefficients.
export function filterResponse(stages: FilterStage[], gain: number) {
    return Array.from({ length: 401 }, (_, index) => ({
        x: index / 400,
        y: filterResponseAt(stages, gain, index / 400),
    }));
}

export function filterResponseAt(stages: FilterStage[], gain: number, x: number): number {
    let level = 0;
    for (const stage of stages) {
        const distance = x - stage.cutoff;
        const width = Math.max(0.008, stage.width);
        const bell = Math.exp(-0.5 * (distance / width) ** 2);
        const slope = 12 + 10 * (stage.order ?? 2);
        const resonance = stage.resonance * 0.28 * Math.exp(-0.5 * (distance / 0.025) ** 2);
        if (stage.kind === 'lowpass') level += -0.62 / (1 + Math.exp(-distance * slope)) + resonance;
        else if (stage.kind === 'highpass') level += -0.62 / (1 + Math.exp(distance * slope)) + resonance;
        else if (stage.kind === 'bandpass') level += (bell - 1) * 0.62;
        else if (stage.kind === 'notch') level -= bell * 0.62;
        else level += bell * stage.resonance * 0.32 * (stage.mix ?? 1);
    }
    return stages.length ? 0.65 + gain * 0.2 + level : 0.65;
}
