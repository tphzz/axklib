export const fixtureFrames = 83996;
export const fixtureRate = 44100;
export function fixturePcm() {
    return Float32Array.from({ length: fixtureFrames }, (_, i) => {
        const hit = i % 10499;
        return Math.sin(hit * 0.2) * Math.exp(-hit / 1900) * (Math.floor(i / 10499) % 2 ? 0.25 : 0.8);
    });
}
export function fixtureWave() {
    const pcm = fixturePcm();
    const bytes = new ArrayBuffer(44 + pcm.length * 2);
    const view = new DataView(bytes);
    const text = (offset: number, value: string) =>
        [...value].forEach((c, i) => view.setUint8(offset + i, c.charCodeAt(0)));
    text(0, 'RIFF');
    view.setUint32(4, bytes.byteLength - 8, true);
    text(8, 'WAVE');
    text(12, 'fmt ');
    view.setUint32(16, 16, true);
    view.setUint16(20, 1, true);
    view.setUint16(22, 1, true);
    view.setUint32(24, fixtureRate, true);
    view.setUint32(28, fixtureRate * 2, true);
    view.setUint16(32, 2, true);
    view.setUint16(34, 16, true);
    text(36, 'data');
    view.setUint32(40, pcm.length * 2, true);
    pcm.forEach((value, i) => view.setInt16(44 + i * 2, Math.round(value * 32767), true));
    return bytes;
}
export function fixtureBins() {
    const pcm = fixturePcm();
    return Array.from({ length: 1024 }, (_, i) => {
        const chunk = pcm.subarray(Math.floor((i * pcm.length) / 1024), Math.floor(((i + 1) * pcm.length) / 1024));
        return { minimum: Math.min(...chunk), maximum: Math.max(...chunk) };
    });
}
