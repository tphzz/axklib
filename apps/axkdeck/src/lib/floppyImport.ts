import type { components } from './generated/axklibApiV1';
import type { InputFileLocation, DirectoryLocation } from './storageLocations';
import type { ImageSessionPackageImportPlan, JobState } from './transport';

export type FloppyInspection = components['schemas']['FloppyImportInspection'];
export type FloppyInputLocation = InputFileLocation | DirectoryLocation;
export type FloppyObject = components['schemas']['FloppyImportObject'];
export type FloppyPlanRequest = Omit<
    components['schemas']['ImageFloppyImportPlanRequest'],
    'imageId' | 'expectedRevision'
>;
export interface FloppyTransport {
    startFloppyInspection(sources: FloppyInputLocation[]): Promise<JobState>;
    releaseFloppyInspection(inspectionToken: string): Promise<void>;
    planFloppyImport(sessionId: number, request: FloppyPlanRequest): Promise<ImageSessionPackageImportPlan>;
    startFloppyImport(planToken: string): Promise<JobState>;
}
