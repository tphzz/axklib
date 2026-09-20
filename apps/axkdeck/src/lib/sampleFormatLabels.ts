import type { SampleStorageFormat } from './objectEditing';

export function sampleConversionTarget(format?: SampleStorageFormat | null): SampleStorageFormat | null {
    return format === 'A3000_188' ? 'A4000_A5000_224' : format === 'A4000_A5000_224' ? 'A3000_188' : null;
}

export function sampleConversionTitle(target?: SampleStorageFormat | null): string {
    return target === 'A3000_188'
        ? 'Convert to a3k sample format'
        : target === 'A4000_A5000_224'
          ? 'Convert to a4k/a5k sample format'
          : 'Convert Sample format';
}
