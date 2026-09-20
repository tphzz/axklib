import type { SampleStorageFormat } from './objectEditing';

export type AudioImportGrouping = { kind: 'SAMPLES' } | { kind: 'SAMPLE_BANK'; sampleBankName: string };
export interface AudioImportOptions {
    sampleFormat: Exclude<SampleStorageFormat, 'UNKNOWN'>;
    grouping: AudioImportGrouping;
}
