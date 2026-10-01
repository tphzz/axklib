export function bufferLevelSummary(buffer: AudioBuffer): { peak: number; rms: number; sampledValues: number } {
    const valuesPerChannel = Math.max(1, Math.floor(32_768 / buffer.numberOfChannels));
    const stride = Math.max(1, Math.floor(buffer.length / valuesPerChannel));
    let peak = 0;
    let squareSum = 0;
    let sampledValues = 0;
    for (let channel = 0; channel < buffer.numberOfChannels; channel += 1) {
        const values = buffer.getChannelData(channel);
        for (let frame = 0; frame < values.length; frame += stride) {
            const value = values[frame] ?? 0;
            peak = Math.max(peak, Math.abs(value));
            squareSum += value * value;
            sampledValues += 1;
        }
    }
    return { peak, rms: sampledValues === 0 ? 0 : Math.sqrt(squareSum / sampledValues), sampledValues };
}
