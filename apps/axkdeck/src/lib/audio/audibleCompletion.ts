export function audibleContextTime(context: AudioContext | undefined): number {
    if (!context) return 0;
    const timestamp = context.getOutputTimestamp?.();
    if (timestamp?.contextTime && timestamp.performanceTime) {
        const elapsed = Math.max(0, performance.now() - timestamp.performanceTime) / 1000;
        return Math.min(context.currentTime, timestamp.contextTime + elapsed);
    }
    const latency = (value: number | undefined) => (Number.isFinite(value) ? Math.max(0, value!) : 0);
    return Math.max(0, context.currentTime - latency(context.baseLatency) - latency(context.outputLatency));
}

// Source completion precedes output-device completion. Keep the visual clock alive
// while queued audio drains, but bound recovery from a stalled device/timestamp.
export function finishAfterOutput(
    endTime: number,
    clock: () => number,
    current: () => boolean,
    finish: () => void,
): () => void {
    let timer: ReturnType<typeof setTimeout> | undefined;
    const remaining = Math.max(0, endTime - clock());
    const deadline = performance.now() + Math.min(2000, remaining * 1000 + 100);
    function poll() {
        if (!current()) return;
        if (clock() >= endTime - 0.00001 || performance.now() >= deadline) finish();
        else timer = setTimeout(poll, 16);
    }
    poll();
    return () => clearTimeout(timer);
}
