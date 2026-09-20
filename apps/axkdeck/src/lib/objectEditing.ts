import type { components } from './generated/axklibApiV1';
import type { JobState } from './transport';

export type SampleEditingSnapshot = components['schemas']['ASeriesSampleEditor'];
export interface ObjectParameterEdit {
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
export interface ObjectEditingTransport {
    startObjectParameterEdit(sessionId: number, edit: ObjectParameterEdit): Promise<JobState>;
    startSampleDuplication(sessionId: number, edit: SampleDuplicationRequest): Promise<JobState>;
    startSampleFormatConversion(sessionId: number, edit: SampleFormatConversionRequest): Promise<JobState>;
}

export type SampleStorageFormat = components['schemas']['SampleStorageFormat'];
export type SampleFormatMetadata = components['schemas']['SampleFormatMetadata'];
export interface SampleFormatConversionRequest {
    expectedRevision: number;
    operation: Omit<ObjectParameterEdit['operation'], 'type' | 'parameters' | 'playback_window'> & {
        type: 'convert_sbnk_format';
        target_format: Lowercase<Exclude<SampleStorageFormat, 'UNKNOWN'>>;
    };
}

export interface SampleDuplicationRequest {
    expectedRevision: number;
    operation: Omit<ObjectParameterEdit['operation'], 'type'> & { type: 'duplicate_sbnk'; new_name: string };
}
