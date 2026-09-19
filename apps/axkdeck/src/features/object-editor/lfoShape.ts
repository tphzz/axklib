export type LfoWave = 'saw' | 'triangle' | 'square' | 'sample-hold';
const onsetPosition = (delay: number) => (Math.max(0, Math.min(127, delay)) / 127) * 0.35;

// Onset, buildup and period are relative guides, not calibrated hardware times.
export function lfoBuildup(delay: number) {
    const onset = onsetPosition(delay);
    return onset === 0
        ? [
              { x: 0, y: 1 },
              { x: 1, y: 1 },
          ]
        : [
              { x: 0, y: 0 },
              { x: onset, y: 0 },
              { x: onset * 2, y: 1 },
              { x: 1, y: 1 },
          ];
}

export function lfoTimeline(wave: LfoWave, speed: number, delay: number, keyOnSync: boolean) {
    const cycles = wave === 'sample-hold' ? 4 : 0.75 + ((Math.max(1, Math.min(128, speed)) - 1) / 127) * 7.25;
    const onset = onsetPosition(delay);
    const levels = [0.3, -0.8, 0.6, 0.1, -0.4, 0.9, -0.2, 0.5];
    const phaseStart = keyOnSync ? 0 : onset * cycles + 0.23;
    const phaseEnd = phaseStart + (1 - onset) * cycles;
    const shape = (phase: number) => {
        const fraction = phase % 1;
        if (wave === 'saw') return 1 - fraction * 2;
        if (wave === 'square') return fraction < 0.5 ? 1 : -1;
        if (wave === 'triangle') return 1 - 4 * Math.abs(((fraction + 0.25) % 1) - 0.5);
        return levels[Math.floor(phase * 2) % levels.length]!;
    };
    const points =
        onset > 0
            ? [
                  { x: 0, y: 0 },
                  { x: onset, y: 0 },
              ]
            : [];
    points.push({ x: onset, y: shape(phaseStart) });
    const step = wave === 'saw' ? 1 : 0.5;
    const offset = wave === 'triangle' ? 0.25 : 0;
    for (let i = Math.floor((phaseStart - offset) / step) + 1; offset + i * step <= phaseEnd; i++) {
        const phase = offset + i * step;
        const x = onset + (phase - phaseStart) / cycles;
        if (wave === 'triangle') points.push({ x, y: i % 2 === 0 ? 1 : -1 });
        else {
            const before = wave === 'saw' ? -1 : shape(phase - 1e-8);
            points.push({ x, y: before }, { x, y: shape(phase) });
        }
    }
    points.push({ x: 1, y: shape(phaseEnd) });
    return points;
}
