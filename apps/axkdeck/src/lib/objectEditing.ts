import type { components } from './generated/axklibApiV1';
import type { JobState } from './transport';

export type SampleEditingSnapshot = components['schemas']['ASeriesSampleEditor'];
export type ProgramEditingSnapshot = components['schemas']['ASeriesProgramEditor'];
export type EditingSnapshot = SampleEditingSnapshot | ProgramEditingSnapshot;
export function sampleSnapshot(
    detail: { editing?: EditingSnapshot | null } | null | undefined,
): SampleEditingSnapshot | undefined {
    return detail?.editing?.profile === 'a-series/program' ? undefined : (detail?.editing ?? undefined);
}
export type ProgramEditorCatalog = components['schemas']['ProgramEditorCatalog'];
export type ProgramEditorFormat = components['schemas']['ProgramEditorFormat'];
export type ProgramEditorTarget = components['schemas']['ProgramEditorTarget'];
export interface ProgramParameterEdit {
    expectedRevision: number;
    operation: {
        id: string;
        partition_index: number;
        volume_name: string;
        program_number: number;
        model: 'A3000' | 'A5000';
        expected_payload_sha256: string;
        parameters: Record<string, unknown>;
    } & (
        | {
              type: 'update_program_parameters';
              assignments: {
                  ordinal: number;
                  expected_target_kind: string;
                  expected_target_name: string;
                  parameters: Record<string, unknown>;
              }[];
          }
        | {
              type: 'replace_program_assignments';
              assignments: {
                  retain_ordinal?: number;
                  sample?: string;
                  sample_bank?: string;
                  parameters?: Record<string, unknown>;
              }[];
          }
    );
}
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
export type ObjectParameterEdit = SampleParameterEdit | BankParameterEdit | ProgramParameterEdit;
export interface ObjectEditingTransport {
    programEditorCatalog(): Promise<ProgramEditorCatalog>;
    startObjectParameterEdit(sessionId: number, edit: ObjectParameterEdit): Promise<JobState>;
    startSampleDuplication(sessionId: number, edit: SampleDuplicationRequest): Promise<JobState>;
    startObjectFormatConversion(sessionId: number, edit: ObjectFormatConversionRequest): Promise<JobState>;
}

export type SampleStorageFormat = components['schemas']['SampleStorageFormat'];
export type SampleFormatMetadata = components['schemas']['SampleFormatMetadata'];
export type ProgramStorageFormat = components['schemas']['ProgramStorageFormat'];
export type ProgramFormatMetadata = components['schemas']['ProgramFormatMetadata'];
export type ObjectStorageFormat = SampleStorageFormat | ProgramStorageFormat;
export type ObjectFormatConversionSnapshot = components['schemas']['ObjectFormatConversion'];
export interface ObjectFormatConversionRequest {
    expectedRevision: number;
    operation: Omit<SampleParameterEdit['operation'], 'type' | 'parameters' | 'playback_window' | 'sample_name'> &
        (
            | ({ target_format: Lowercase<Exclude<SampleStorageFormat, 'UNKNOWN'>> } & (
                  | { type: 'convert_sbnk_format'; sample_name: string }
                  | { type: 'convert_sbac_format'; sample_bank_name: string }
              ))
            | {
                  type: 'convert_prog_format';
                  program_number: number;
                  target_format: Lowercase<Exclude<ProgramStorageFormat, 'UNKNOWN'>>;
              }
        );
}

export interface SampleDuplicationRequest {
    expectedRevision: number;
    operation: Omit<SampleParameterEdit['operation'], 'type'> & { type: 'duplicate_sbnk'; new_name: string };
}
