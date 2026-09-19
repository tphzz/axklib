import type { components } from './generated/axklibApiV1';
import type { JobState } from './transport';

export type SampleEditingSnapshot = components['schemas']['A4000A5000SampleEditor'];
export interface ObjectParameterEdit {
    expectedRevision: number;
    operation: {
        id: string;
        type: 'update_sbnk_parameters';
        partition_index: number;
        volume_name: string;
        sample_name: string;
        expected_payload_sha256: string;
        parameters: components['schemas']['A4000A5000SampleParameters'];
        playback_window?: components['schemas']['SamplePlaybackWindow'];
    };
}
export interface ObjectEditingTransport {
    startObjectParameterEdit(sessionId: number, edit: ObjectParameterEdit): Promise<JobState>;
}
