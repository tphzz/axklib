interface RiffWaveFormat {
    encoding: 'pcm' | 'float';
    channels: number;
    sampleRate: number;
    blockAlign: number;
    bitsPerSample: number;
}

interface RiffWaveLayout {
    format: RiffWaveFormat;
    dataOffset: number;
    dataBytes: number;
}

function matches(bytes: Uint8Array, offset: number, expected: string): boolean {
    if (offset + expected.length > bytes.length) return false;
    for (let index = 0; index < expected.length; ++index) {
        if (bytes[offset + index] !== expected.charCodeAt(index)) return false;
    }
    return true;
}

function extensibleEncoding(
    view: DataView,
    offset: number,
    size: number,
    bitsPerSample: number,
): 'pcm' | 'float' | null {
    if (size < 40 || view.getUint16(offset + 16, true) < 22) return null;
    const validBits = view.getUint16(offset + 18, true);
    if (validBits !== 0 && validBits !== bitsPerSample) return null;
    if (
        view.getUint16(offset + 28, true) !== 0 ||
        view.getUint16(offset + 30, true) !== 0x0010 ||
        view.getUint32(offset + 32, false) !== 0x800000aa ||
        view.getUint32(offset + 36, false) !== 0x00389b71
    ) {
        return null;
    }
    const tag = view.getUint32(offset + 24, true);
    return tag === 1 ? 'pcm' : tag === 3 ? 'float' : null;
}

function parseFormat(view: DataView, offset: number, size: number): RiffWaveFormat | null {
    if (size < 16) return null;
    const tag = view.getUint16(offset, true);
    const channels = view.getUint16(offset + 2, true);
    const sampleRate = view.getUint32(offset + 4, true);
    const blockAlign = view.getUint16(offset + 12, true);
    const bitsPerSample = view.getUint16(offset + 14, true);
    const encoding =
        tag === 1
            ? 'pcm'
            : tag === 3
              ? 'float'
              : tag === 0xfffe
                ? extensibleEncoding(view, offset, size, bitsPerSample)
                : null;
    const supportedBits = encoding === 'pcm' ? [8, 16, 24, 32] : encoding === 'float' ? [32, 64] : [];
    const bytesPerSample = bitsPerSample / 8;
    if (
        encoding === null ||
        channels === 0 ||
        channels > 32 ||
        sampleRate === 0 ||
        !supportedBits.includes(bitsPerSample) ||
        !Number.isInteger(bytesPerSample) ||
        blockAlign < channels * bytesPerSample
    ) {
        return null;
    }
    return { encoding, channels, sampleRate, blockAlign, bitsPerSample };
}

function parseLayout(content: ArrayBuffer): RiffWaveLayout | null {
    if (content.byteLength < 12) return null;
    const bytes = new Uint8Array(content);
    const view = new DataView(content);
    if (!matches(bytes, 0, 'RIFF') || !matches(bytes, 8, 'WAVE')) return null;
    const riffBytes = view.getUint32(4, true) + 8;
    if (riffBytes < 12 || riffBytes > content.byteLength) return null;

    let format: RiffWaveFormat | null = null;
    let dataOffset = 0;
    let dataBytes = 0;
    let offset = 12;
    while (offset + 8 <= riffBytes) {
        const size = view.getUint32(offset + 4, true);
        const payloadOffset = offset + 8;
        if (size > riffBytes - payloadOffset) return null;
        if (!format && matches(bytes, offset, 'fmt ')) format = parseFormat(view, payloadOffset, size);
        if (dataOffset === 0 && matches(bytes, offset, 'data')) {
            dataOffset = payloadOffset;
            dataBytes = size;
        }
        offset = payloadOffset + size + (size % 2);
    }
    return format && dataOffset !== 0 ? { format, dataOffset, dataBytes } : null;
}

function pcmSample(view: DataView, offset: number, bits: number): number {
    if (bits === 8) return (view.getUint8(offset) - 128) / 128;
    if (bits === 16) return view.getInt16(offset, true) / 32_768;
    if (bits === 32) return view.getInt32(offset, true) / 2_147_483_648;
    let value = view.getUint8(offset) | (view.getUint8(offset + 1) << 8) | (view.getUint8(offset + 2) << 16);
    if ((value & 0x800000) !== 0) value |= 0xff000000;
    return value / 8_388_608;
}

export function decodeRiffWavePcm(context: BaseAudioContext, content: ArrayBuffer): AudioBuffer | null {
    const layout = parseLayout(content);
    if (!layout || layout.dataBytes % layout.format.blockAlign !== 0) return null;
    const frameCount = layout.dataBytes / layout.format.blockAlign;
    if (frameCount === 0) return null;
    let buffer: AudioBuffer;
    try {
        buffer = context.createBuffer(layout.format.channels, frameCount, layout.format.sampleRate);
    } catch {
        return null;
    }
    const view = new DataView(content);
    const bytesPerSample = layout.format.bitsPerSample / 8;
    const channels = Array.from({ length: layout.format.channels }, (_, channel) => ({
        offset: channel * bytesPerSample,
        data: buffer.getChannelData(channel),
    }));
    for (let frame = 0; frame < frameCount; ++frame) {
        const frameOffset = layout.dataOffset + frame * layout.format.blockAlign;
        for (const channel of channels) {
            const offset = frameOffset + channel.offset;
            const value =
                layout.format.encoding === 'pcm'
                    ? pcmSample(view, offset, layout.format.bitsPerSample)
                    : layout.format.bitsPerSample === 32
                      ? view.getFloat32(offset, true)
                      : view.getFloat64(offset, true);
            channel.data[frame] = Number.isFinite(value) ? value : 0;
        }
    }
    return buffer;
}
