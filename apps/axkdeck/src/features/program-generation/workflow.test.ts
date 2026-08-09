import { describe, expect, it, vi } from 'vitest';

import type { ImageTransport, ProgramGenerationInspection } from '../../lib/transport';
import { ProgramGenerationWorkflow } from './workflow.svelte';

const volume = {
    id: 'volume-1',
    name: 'Drums',
    kind: 'volume' as const,
    childCount: 3,
    partitionIndex: 0,
};

function inspection(overrides: Partial<ProgramGenerationInspection> = {}): ProgramGenerationInspection {
    return {
        imageId: 'image-1',
        revision: 4,
        contentScopeId: 'volume-1',
        availableProgramNumbers: [1, 3],
        candidates: [
            {
                targetObjectId: 'bank-1',
                targetObjectName: 'Drum Bank',
                targetObjectType: 'SBAC',
                defaultProgramName: 'DrumBank',
                defaultSelected: true,
                programNumber: 1,
            },
            {
                targetObjectId: 'sample-1',
                targetObjectName: 'Kick',
                targetObjectType: 'SBNK',
                defaultProgramName: 'Kick',
                defaultSelected: true,
                programNumber: 3,
            },
            {
                targetObjectId: 'sample-2',
                targetObjectName: 'Snare',
                targetObjectType: 'SBNK',
                defaultProgramName: 'Snare',
                defaultSelected: false,
                programNumber: null,
            },
        ],
        notices: [],
        ...overrides,
    };
}

function workflowWith(inspections: ProgramGenerationInspection[]) {
    const inspectProgramGeneration = vi.fn();
    for (const value of inspections) inspectProgramGeneration.mockResolvedValueOnce(value);
    const startProgramGeneration = vi
        .fn()
        .mockResolvedValue({ jobId: 9, kind: 'PROGRAM_GENERATION', status: 'queued' });
    const refreshSession = vi.fn().mockResolvedValue(undefined);
    const selectProgram = vi.fn();
    const selectWorkspace = vi.fn();
    const invalidateSession = vi.fn().mockResolvedValue(undefined);
    const setStatus = vi.fn();
    const reportTiming = vi.fn();
    const run = vi.fn().mockImplementation(async (start: () => Promise<unknown>) => {
        await start();
        return {
            jobId: 9,
            kind: 'PROGRAM_GENERATION',
            status: 'completed',
            result: {
                kind: 'PROGRAM_GENERATION',
                applied: true,
                createdPrograms: [
                    {
                        programNumber: 1,
                        programName: 'Drums',
                        targetObjectId: 'bank-1',
                        targetObjectName: 'Drum Bank',
                        targetObjectType: 'SBAC',
                    },
                ],
            },
        };
    });
    const workflow = new ProgramGenerationWorkflow({
        transport: { inspectProgramGeneration, startProgramGeneration } as unknown as ImageTransport,
        jobs: { run } as never,
        sessionId: () => 7,
        activeVolumeId: () => 'volume-1',
        selectedSource: () => volume,
        refreshSession,
        invalidateSession,
        selectWorkspace,
        selectProgram,
        setStatus,
        reportTiming,
    });
    return {
        workflow,
        inspectProgramGeneration,
        startProgramGeneration,
        refreshSession,
        invalidateSession,
        selectWorkspace,
        selectProgram,
        setStatus,
        reportTiming,
    };
}

describe('ProgramGenerationWorkflow', () => {
    it('packs selected candidates into free Program slots and generates the reviewed Programs', async () => {
        const context = workflowWith([inspection(), inspection()]);

        await context.workflow.open();
        expect(
            context.workflow.request?.rows.map((row) => [row.targetObjectName, row.selected, row.programNumber]),
        ).toEqual([
            ['Drum Bank', true, 1],
            ['Kick', true, 3],
            ['Snare', false, null],
        ]);

        context.workflow.setSelected('bank-1', false);
        context.workflow.setSelected('sample-2', true);
        context.workflow.setProgramName('sample-1', 'Kick One');
        expect(
            context.workflow.request?.rows.map((row) => [row.targetObjectName, row.selected, row.programNumber]),
        ).toEqual([
            ['Drum Bank', false, null],
            ['Kick', true, 1],
            ['Snare', true, 3],
        ]);

        await context.workflow.submit();

        expect(context.startProgramGeneration).toHaveBeenCalledWith(7, 'volume-1', [
            { targetObjectId: 'sample-1', programNumber: 1, programName: 'Kick One' },
            { targetObjectId: 'sample-2', programNumber: 3, programName: 'Snare' },
        ]);
        expect(context.invalidateSession).toHaveBeenCalledWith(7);
        expect(context.refreshSession).toHaveBeenCalledWith({ partitionIndex: 0, volumeName: 'Drums' });
        expect(context.selectWorkspace).toHaveBeenCalledWith('programs');
        expect(context.selectProgram).toHaveBeenCalledWith(1);
        expect(context.workflow.request).toBeNull();
    });

    it('selects only the candidates that fit when all rows are checked', async () => {
        const { workflow } = workflowWith([inspection()]);
        await workflow.open();

        workflow.setAllSelected(false);
        workflow.setAllSelected(true);

        expect(workflow.request?.rows.map((row) => [row.selected, row.programNumber])).toEqual([
            [true, 1],
            [true, 3],
            [false, null],
        ]);
    });

    it('refuses to apply when the final inspection no longer matches the reviewed candidates', async () => {
        const changed = inspection({ availableProgramNumbers: [1] });
        const context = workflowWith([inspection(), changed]);
        await context.workflow.open();

        await context.workflow.submit();

        expect(context.startProgramGeneration).not.toHaveBeenCalled();
        expect(context.workflow.request?.error).toContain('changed');
        expect(context.workflow.request?.rows.filter((row) => row.selected)).toHaveLength(1);
    });
});
