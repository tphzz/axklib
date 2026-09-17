import type { components } from './generated/axklibApiV1';
import type { InputFileLocation } from './storageLocations';
import type { JobState } from './transport';

export type Su700Inspection = components['schemas']['Su700ImportInspection'];
export type Su700ImportResult = components['schemas']['Su700ImportResult'];
export interface Su700Request {
    source: InputFileLocation;
    destination: { sessionId: number; expectedRevision: number; rootEntryId: string; volumeName: string } | null;
    includedExtras: string[] | null;
    expectedSource?: Su700Inspection['snapshot'];
    idempotencyKey?: string;
}
export interface Su700Transport {
    startSu700Import(request: Su700Request): Promise<JobState>;
}
