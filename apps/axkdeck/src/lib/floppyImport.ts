import type { components } from './generated/axklibApiV1';
import type { InputFileLocation } from './storageLocations';
import type { ImageSessionPackageImportPlan, JobState } from './transport';

export type FloppyInspection = components['schemas']['FloppyImportInspection'];
export type FloppyObject = components['schemas']['FloppyImportObject'];
export type FloppyPlanRequest = Omit<
    components['schemas']['ImageFloppyImportPlanRequest'],
    'imageId' | 'expectedRevision'
>;
export interface FloppyTransport {
    startFloppyInspection(sources: InputFileLocation[]): Promise<JobState>;
    releaseFloppyInspection(inspectionToken: string): Promise<void>;
    planFloppyImport(sessionId: number, request: FloppyPlanRequest): Promise<ImageSessionPackageImportPlan>;
    startFloppyImport(planToken: string): Promise<JobState>;
}
