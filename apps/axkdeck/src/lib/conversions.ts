import type { components } from './generated/axklibApiV1';
import type { DirectoryLocation, InputFileLocation } from './storageLocations';
import type { CapacityPolicy } from './importCapacity';
import type { JobState } from './transport';

export type ConversionPlan = components['schemas']['ConversionPlan'];
export type ConversionProfile = components['schemas']['ConversionProfile'];
export type ConversionReport = components['schemas']['ConversionReport'];
export type ConversionSourceObject = NonNullable<ConversionReport['sourceObjects']>[number];
export type ConversionFidelity = ConversionReport['fidelity'];
export type CompanionSourceResolution = components['schemas']['CompanionSourceResolution'];
export interface CompanionSourceInput {
    formatId?: components['schemas']['CompanionSourceResolutionRequest']['formatId'];
    sources: InputFileLocation[];
    candidates?: InputFileLocation[];
    exclusions?: InputFileLocation[];
    searchNearby?: boolean;
}
export type ConversionSource =
    | { kind: 'DOCUMENT'; sessionId: number; documentId: string; expectedRevision: number }
    | { kind: 'SOURCES'; sources: (InputFileLocation | DirectoryLocation)[] };
export type ConversionInput = {
    profileId: string;
    source: ConversionSource;
    options: Record<string, unknown>;
    fidelity: ConversionFidelity;
} & (
    | { destinationSessionId: number; selection: Record<string, unknown>; capacityPolicy: CapacityPolicy }
    | { destination: { kind: 'NEW_IMAGE' } }
);
export interface ConversionTransport {
    resolveCompanionSources(input: CompanionSourceInput, signal?: AbortSignal): Promise<CompanionSourceResolution>;
    conversionProfiles(): Promise<ConversionProfile[]>;
    prepareConversion(input: ConversionInput, signal?: AbortSignal): Promise<ConversionPlan>;
    conversionPlan(planId: string, offset?: number): Promise<ConversionPlan>;
    releaseConversion(planId: string): Promise<void>;
    executeConversion(plan: ConversionPlan, acknowledgedIssueIds: string[]): Promise<JobState>;
    conversionExecution(planId: string): Promise<JobState | null>;
}

export function requiredConversionIssues(plan: ConversionPlan): string[] {
    return plan.report?.issues.filter((issue) => !!issue.decision).map((issue) => issue.id) ?? [];
}
