import type { components } from './generated/axklibApiV1';
import type { JobState } from './transport';

export type SampleEditingSnapshot = components['schemas']['ASeriesSampleEditor'];
export interface SampleParameterEdit {
    expectedRevision: number;
    operation: {
        id: string;
        type: 'update_sbnk_parameters';
        partition_index: number;
        volume_name: string;
        sample_name: string;
        expected_payload_sha256: string;
        parameters: components['schemas']['ASeriesSampleParameters'];
        playback_window?: components['schemas']['SamplePlaybackWindow'];
    };
}
export interface BankParameterEdit {
    expectedRevision: number;
    operation: {
        id: string;
        type: 'update_sample_bank_overrides';
        partition_index: number;
        volume_name: string;
        sample_bank_name: string;
        expected_payload_sha256: string;
        parameters: components['schemas']['ASeriesSampleParameters'];
        enable: number[];
        disable: number[];
    };
}
export type ObjectParameterEdit = SampleParameterEdit | BankParameterEdit;
export interface ObjectEditingTransport {
    startObjectParameterEdit(sessionId: number, edit: ObjectParameterEdit): Promise<JobState>;
    startSampleDuplication(sessionId: number, edit: SampleDuplicationRequest): Promise<JobState>;
    startObjectFormatConversion(sessionId: number, edit: ObjectFormatConversionRequest): Promise<JobState>;
}

export type SampleStorageFormat = components['schemas']['SampleStorageFormat'];
export type SampleFormatMetadata = components['schemas']['SampleFormatMetadata'];
export type ObjectFormatConversionSnapshot = components['schemas']['ObjectFormatConversion'];
export interface ObjectFormatConversionRequest {
    expectedRevision: number;
    operation: Omit<SampleParameterEdit['operation'], 'type' | 'parameters' | 'playback_window' | 'sample_name'> & {
        target_format: Lowercase<Exclude<SampleStorageFormat, 'UNKNOWN'>>;
    } & (
            | { type: 'convert_sbnk_format'; sample_name: string }
            | { type: 'convert_sbac_format'; sample_bank_name: string }
        );
}

export interface SampleDuplicationRequest {
    expectedRevision: number;
    operation: Omit<SampleParameterEdit['operation'], 'type'> & { type: 'duplicate_sbnk'; new_name: string };
}
