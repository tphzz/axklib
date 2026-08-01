export function formatSequenceTempo(microsecondsPerQuarterNote: number): string {
    const beatsPerMinute = 60_000_000 / microsecondsPerQuarterNote;
    const rounded = Math.round(beatsPerMinute * 10) / 10;
    return `${rounded.toLocaleString(undefined, {
        minimumFractionDigits: Number.isInteger(rounded) ? 0 : 1,
        maximumFractionDigits: 1,
    })} BPM`;
}

export function laterTempoChangeCount(tempoEvents: readonly { tick: number }[]): number {
    return tempoEvents.filter((event) => event.tick > 0).length;
}
