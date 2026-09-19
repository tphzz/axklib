// Mirrors library/src/sample_parameter_codec.cpp; shared native reference vectors test parity.
const frequencyBits = [
    0x3ababeff, 0x3acd6bb4, 0x3ae96ec5, 0x3b02b8eb, 0x3b1565a3, 0x3b28125d, 0x3b3abf18, 0x3b521705, 0x3b696ef5,
    0x3b82b90e, 0x3b93103d, 0x3ba36771, 0x3bbabf7b, 0x3bd21792, 0x3be96fb7, 0x3c0063f6, 0x3c11e62e, 0x3c23687b,
    0x3c3ac109, 0x3c5219c8, 0x3c6972c0, 0x3c835127, 0x3c91e924, 0x3ca36ca5, 0x3cb7dbe2, 0x3ccf3729, 0x3ce97ee3,
    0x3d0359cb, 0x3d11f4ff, 0x3d237d4d, 0x3d37f39a, 0x3d4c6c41, 0x3d69af7d, 0x3d837c68, 0x3d922480, 0x3da0d069,
    0x3daf8086, 0x3dcceeed, 0x3dea72d8, 0x3e0407ba, 0x3e12e3fb, 0x3e21cfd2, 0x3e38526e, 0x3e4f018e, 0x3e6d8fd5,
    0x3e864320, 0x3e95fa07, 0x3ea9fee7, 0x3ebe7e80, 0x3ed7d37b, 0x3ef690f3, 0x3f0b6901, 0x3f240d96, 0x3f3f0eca,
    0x3f5d19c1, 0x3f7f16fb, 0x3f93242a, 0x3fc62eca, 0x400b2f66, 0x4057a322, 0x40d98fbc,
];
const gainBits = [
    0x3f800000, 0x3f8f9e4d, 0x3fa12478, 0x3fb4ce08, 0x3fcaddc8, 0x3fe39ea9, 0x3fff64c2, 0x400f4736, 0x4020c2bf,
    0x40346063, 0x404a62c2, 0x406314a1, 0x407ec9e2,
];
const rootBits = [
    0x3f800000, 0x3f8795a0, 0x3f8f9e4d, 0x3f9820d7, 0x3fa12478, 0x3faab0d5, 0x3fb4ce08, 0x3fbf84a7, 0x3fcaddc8,
    0x3fd6e30d, 0x3fe39ea9, 0x3ff11b6a, 0x3fff64c2,
];
export const eqFrequencies = [
    32, 36, 40, 45, 50, 56, 63, 70, 80, 90, 100, 110, 125, 140, 160, 180, 200, 225, 250, 280, 315, 355, 400, 450, 500,
    560, 630, 700, 800, 900, 1000, 1100, 1200, 1400, 1600, 1800, 2000, 2200, 2500, 2800, 3200, 3600, 4000, 4500, 5000,
    5600, 6300, 7000, 8000, 9000, 10000, 11000, 12000, 14000, 16000,
];
const float = (bits: number) => {
    const buffer = new DataView(new ArrayBuffer(4));
    buffer.setUint32(0, bits);
    return buffer.getFloat32(0);
};
const frequencyFactors = frequencyBits.map(float);
const gainFactors = gainBits.map(float);
const gainRoots = rootBits.map(float);
const quantize = (value: number) =>
    Math.max(-32768, Math.min(32767, Math.sign(value) * Math.floor(Math.abs(value * 8192) + 0.5)));
export const eqFrequencyPosition = (selection: number) => Math.log(eqFrequencies[selection - 4]! / 32) / Math.log(500);
export const eqFrequencyLabel = (selection: number) => {
    const hz = eqFrequencies[selection - 4]!;
    return hz < 1000 ? `${hz} Hz` : `${hz / 1000} kHz`;
};
export function eqFrequencySelection(x: number, current?: number) {
    if (current !== undefined && current >= 4 && current <= 58) {
        const center = eqFrequencyPosition(current);
        const lower = current === 4 ? -Infinity : center - (center - eqFrequencyPosition(current - 1)) * 0.6;
        const upper = current === 58 ? Infinity : center + (eqFrequencyPosition(current + 1) - center) * 0.6;
        if (x >= lower && x <= upper) return current;
    }
    let index = 4;
    for (let candidate = 5; candidate <= 58; candidate++)
        if (Math.abs(eqFrequencyPosition(candidate) - x) < Math.abs(eqFrequencyPosition(index) - x)) index = candidate;
    return index;
}
export function eqEffectiveGain(type: number, frequency: number, gain: number) {
    const limit =
        frequency < 33
            ? 6
            : frequency < 39
              ? 7
              : frequency < 43
                ? 8
                : frequency < 45
                  ? 9
                  : frequency < 47
                    ? 10
                    : frequency < 49
                      ? 11
                      : 12;
    return type === 2 ? Math.max(-limit, Math.min(limit, gain)) : gain;
}
export function eqCoefficients(type: number, frequency: number, gain: number, width: number): number[] {
    return eqParameterCoefficients(type, frequency, gain, width).map(quantize);
}
// Pre-quantization coefficients are only an editing model, not stored or measured hardware response.
export function eqParameterCoefficients(type: number, frequency: number, gain: number, width: number): number[] {
    gain = eqEffectiveGain(type, frequency, gain);
    const f = frequencyFactors[frequency]!,
        f2 = f * f;
    const g = gainFactors[Math.abs(gain)]!,
        root = gainRoots[Math.abs(gain)]!;
    const peak = (10 * f) / width,
        shelf = 2 * f;
    let n, b0, b1, b2, a1, a2;
    if (type === 0 && gain >= 0) {
        n = 1 / (1 + peak + f2);
        b0 = (1 + g * peak + f2) * n;
        b1 = 2 * (f2 - 1) * n;
        b2 = (1 - g * peak + f2) * n;
        a1 = -b1;
        a2 = -(1 - peak + f2) * n;
    } else if (type === 0) {
        n = 1 / (1 + g * peak + f2);
        b0 = (1 + peak + f2) * n;
        b1 = 2 * (f2 - 1) * n;
        b2 = (1 - peak + f2) * n;
        a1 = -b1;
        a2 = -(1 - g * peak + f2) * n;
    } else if (type === 1 && gain >= 0) {
        n = 1 / (1 + shelf + f2);
        b0 = (1 + root * shelf + g * f2) * n;
        b1 = 2 * (g * f2 - 1) * n;
        b2 = (1 - root * shelf + g * f2) * n;
        a1 = -2 * (f2 - 1) * n;
        a2 = -(1 - shelf + f2) * n;
    } else if (type === 1) {
        n = 1 / (1 + root * shelf + g * f2);
        b0 = (1 + shelf + f2) * n;
        b1 = 2 * (f2 - 1) * n;
        b2 = (1 - shelf + f2) * n;
        a1 = -2 * (g * f2 - 1) * n;
        a2 = -(1 - root * shelf + g * f2) * n;
    } else if (gain >= 0) {
        n = 1 / (1 + shelf + f2);
        b0 = (g + root * shelf + f2) * n;
        b1 = 2 * (f2 - g) * n;
        b2 = (g - root * shelf + f2) * n;
        a1 = -2 * (f2 - 1) * n;
        a2 = -(1 - shelf + f2) * n;
    } else {
        n = 1 / (g + root * shelf + f2);
        b0 = (1 + shelf + f2) * n;
        b1 = 2 * (f2 - 1) * n;
        b2 = (1 - shelf + f2) * n;
        a1 = -2 * (f2 - g) * n;
        a2 = -(g - root * shelf + f2) * n;
    }
    return [b1, b2, b0, a1, a2];
}
export function eqResponse(coefficients: number[], x: number) {
    return eqParameterResponse(
        coefficients.map((value) => value / 8192),
        x,
    );
}
export function eqParameterResponse(coefficients: number[], x: number) {
    let selection = 4;
    while (selection < 57 && eqFrequencyPosition(selection + 1) < x) selection++;
    const fraction = Math.max(
        0,
        Math.min(
            1,
            (x - eqFrequencyPosition(selection)) /
                (eqFrequencyPosition(selection + 1) - eqFrequencyPosition(selection)),
        ),
    );
    // Interpolate in the maintained frequency-factor table, not at the source WAV sample rate.
    const factor = Math.exp(
        Math.log(frequencyFactors[selection]!) * (1 - fraction) + Math.log(frequencyFactors[selection + 1]!) * fraction,
    );
    const omega = 2 * Math.atan(factor);
    const [b1, b2, b0, a1, a2] = coefficients as [number, number, number, number, number];
    const power = (a: number, b: number, c: number) =>
        (a + b * Math.cos(omega) + c * Math.cos(2 * omega)) ** 2 + (b * Math.sin(omega) + c * Math.sin(2 * omega)) ** 2;
    return 10 * Math.log10(Math.max(1e-12, power(b0, b1, b2)) / Math.max(1e-12, power(1, -a1, -a2)));
}
