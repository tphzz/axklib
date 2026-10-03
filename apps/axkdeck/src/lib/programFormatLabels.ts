import type { ProgramStorageFormat } from './objectEditing';

export function programConversionTarget(format?: ProgramStorageFormat | null): ProgramStorageFormat | null {
    return format === 'A3000' ? 'A4000_A5000' : format === 'A4000_A5000' ? 'A3000' : null;
}

export function programConversionTitle(target?: ProgramStorageFormat | null): string {
    return target === 'A3000'
        ? 'Convert to a3k program format'
        : target === 'A4000_A5000'
          ? 'Convert to a4k/a5k program format'
          : 'Convert Program format';
}
