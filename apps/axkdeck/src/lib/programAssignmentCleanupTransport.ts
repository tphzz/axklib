import type { components } from './generated/axklibApiV1';
import type { JobState } from './transport';

export type ProgramAssignmentCleanupCandidate = components['schemas']['ImageProgramAssignmentCleanupCandidate'];
export type ProgramAssignmentCleanupInspection = components['schemas']['ImageProgramAssignmentCleanupInspection'];
export type ProgramAssignmentCleanupSelection = components['schemas']['ImageProgramAssignmentCleanupSelection'];
export type ProgramAssignmentCleanupResult = components['schemas']['ImageProgramAssignmentCleanupResult'];

export interface ProgramAssignmentCleanupTransport {
    inspectProgramAssignmentCleanup(
        sessionId: number,
        contentScopeId: string,
    ): Promise<ProgramAssignmentCleanupInspection>;
    startProgramAssignmentCleanup(
        sessionId: number,
        contentScopeId: string,
        assignments: ProgramAssignmentCleanupSelection[],
    ): Promise<JobState>;
}
