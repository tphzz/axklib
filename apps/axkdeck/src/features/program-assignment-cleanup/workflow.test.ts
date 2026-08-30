import { describe, expect, it, vi } from 'vitest';

import type { ImageTransport, ProgramAssignmentCleanupInspection } from '../../lib/transport';
import { ProgramAssignmentCleanupWorkflow } from './workflow.svelte';

const volume = { id: 'volume-1', name: 'Drums', kind: 'volume' as const, childCount: 3, partitionIndex: 0 };

function inspection(name = 'Missing Bank'): ProgramAssignmentCleanupInspection {
    return {
        imageId: 'image-1',
        revision: 4,
        contentScopeId: 'volume-1',
        totalCandidateCount: 2,
        candidates: [
            {
                programObjectId: 'program-1',
                programNumber: 1,
                programName: 'Kit',
                assignmentOrdinal: 0,
                assignmentName: name,
                targetObjectType: 'SBAC',
                receiveChannelDisplay: '=Smp',
                reason: 'MISSING_TARGET',
                candidateTargetCount: 0,
                defaultSelected: true,
            },
            {
                programObjectId: 'program-1',
                programNumber: 1,
                programName: 'Kit',
                assignmentOrdinal: 2,
                assignmentName: 'Old Snare',
                targetObjectType: 'SBNK',
                receiveChannelDisplay: 'A02',
                reason: 'AMBIGUOUS_TARGET',
                candidateTargetCount: 2,
                defaultSelected: true,
            },
        ],
    };
}

function setup(inspections: ProgramAssignmentCleanupInspection[]) {
    const inspectProgramAssignmentCleanup = vi.fn();
    for (const value of inspections) inspectProgramAssignmentCleanup.mockResolvedValueOnce(value);
    const startProgramAssignmentCleanup = vi.fn().mockResolvedValue({ jobId: 1, status: 'queued' });
    const refreshSession = vi.fn().mockResolvedValue(undefined);
    const invalidateSession = vi.fn().mockResolvedValue(undefined);
    const setStatus = vi.fn();
    const run = vi.fn().mockImplementation(async (start: () => Promise<unknown>) => {
        await start();
        return { jobId: 1, status: 'completed', result: { kind: 'PROGRAM_ASSIGNMENT_CLEANUP' } };
    });
    const workflow = new ProgramAssignmentCleanupWorkflow({
        transport: { inspectProgramAssignmentCleanup, startProgramAssignmentCleanup } as unknown as ImageTransport,
        jobs: { run } as never,
        sessionId: () => 7,
        activeVolumeId: () => 'volume-1',
        selectedSource: () => volume,
        refreshSession,
        invalidateSession,
        setStatus,
        reportTiming: vi.fn(),
    });
    return { workflow, inspectProgramAssignmentCleanup, startProgramAssignmentCleanup, refreshSession };
}

describe('ProgramAssignmentCleanupWorkflow', () => {
    it('selects all by default, supports Program-level selection, and cleans the reviewed rows', async () => {
        const context = setup([inspection(), inspection()]);
        await context.workflow.open();
        expect(context.workflow.request?.rows.every((row) => row.selected)).toBe(true);

        context.workflow.setProgramSelected('program-1', false);
        expect(context.workflow.request?.rows.every((row) => !row.selected)).toBe(true);
        context.workflow.setAssignmentSelected('program-1', 2, true);
        await context.workflow.submit();

        expect(context.startProgramAssignmentCleanup).toHaveBeenCalledWith(7, 'volume-1', [
            { programObjectId: 'program-1', assignmentOrdinal: 2 },
        ]);
        expect(context.refreshSession).toHaveBeenCalledWith({ partitionIndex: 0, volumeName: 'Drums' });
        expect(context.workflow.request).toBeNull();
    });

    it('requires another review when final inspection changes', async () => {
        const context = setup([inspection(), inspection('Different Missing Bank')]);
        await context.workflow.open();
        await context.workflow.submit();

        expect(context.startProgramAssignmentCleanup).not.toHaveBeenCalled();
        expect(context.workflow.request?.error).toContain('changed');
        expect(context.workflow.request?.rows.every((row) => row.selected)).toBe(true);
    });
});
