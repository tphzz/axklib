import type { components } from './generated/axklibApiV1';
import type { DirectoryLocation, InputFileLocation } from './storageLocations';
import type { CapacityPolicy } from './importCapacity';
import type { JobState } from './transport';

export type ConversionPlan = components['schemas']['ConversionPlan'];
export type ConversionProfile = components['schemas']['ConversionProfile'];
export type ConversionReport = components['schemas']['ConversionReport'];
export type ConversionFidelity = ConversionReport['fidelity'];
export type ConversionSource =
    | { kind: 'DOCUMENT'; sessionId: number; documentId: string; expectedRevision: number }
    | { kind: 'SOURCES'; sources: (InputFileLocation | DirectoryLocation)[] };
export interface ConversionInput {
    profileId: string;
    source: ConversionSource;
    destinationSessionId: number;
    selection: Record<string, unknown>;
    options: Record<string, unknown>;
    fidelity: ConversionFidelity;
    capacityPolicy: CapacityPolicy;
}
export interface ConversionTransport {
    conversionProfiles(): Promise<ConversionProfile[]>;
    prepareConversion(input: ConversionInput, signal?: AbortSignal): Promise<ConversionPlan>;
    conversionPlan(planId: string, offset?: number): Promise<ConversionPlan>;
    releaseConversion(planId: string): Promise<void>;
    executeConversion(plan: ConversionPlan, acknowledgedIssueIds: string[]): Promise<JobState>;
    conversionExecution(planId: string): Promise<JobState | null>;
}

export function requiredConversionIssues(plan: ConversionPlan): string[] {
    return (
        plan.report?.issues
            .filter((issue) => ['LOSS', 'UNCERTAINTY'].includes(issue.impact))
            .map((issue) => issue.id) ?? []
    );
}
