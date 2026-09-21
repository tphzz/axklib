import { describe, expect, it, vi } from 'vitest';

import type { SampleFormatMetadata, SampleStorageFormat } from '../../lib/objectEditing';
import type { ImageTransport } from '../../lib/transport';
import type { SampleStructureItem } from '../../lib/types';
import { MutationWorkflow } from './workflow.svelte';

function metadata(format: SampleStorageFormat): SampleFormatMetadata {
    return {
        format,
        structurallyValid: format !== 'UNKNOWN',
        headerRevision: format === 'A3000_188' ? 2 : format === 'A4000_A5000_224' ? 4 : 0,
        parameterBytes: format === 'A3000_188' ? 188 : format === 'A4000_A5000_224' ? 224 : null,
        olderBodyBytes: 300,
        laterBodyBytes: 0,
        extensionDiffersFromPrefixDefaults: null,
        requiresA5000: false,
        parameterIssues: [],
        diagnostics: [],
    };
}

function sample(name: string, format?: SampleStorageFormat): SampleStructureItem {
    return {
        id: name,
        objectId: name,
        name,
        objectType: 'SBNK',
        sampleBankObjectIds: [],
        object: {
            key: name,
            objectType: 'SBNK',
            name,
            partitionIndex: 0,
            partitionName: 'Partition 1',
            volumeName: 'Samples',
            categoryName: 'SBNK',
            objectEncoding: 'current',
            directoryEntryName: `${name}.001`,
            sfsId: 1,
            storedSizeBytes: 512,
            sizeWithDependenciesBytes: null,
            sampleRate: 44_100,
            rootKey: 60,
            storedFrameCount: 1,
            waveStartFrame: 0,
            waveLengthFrames: 1,
            storageState: 'COMPLETE',
            sampleWidthBytes: 2,
            sampleFormat: format === undefined ? null : metadata(format),
        },
    };
}

function createWorkflow(preferredGeneration?: 'A3000' | 'A4000_A5000', banks: SampleStructureItem[] = []) {
    const startSampleBankCreation = vi.fn().mockResolvedValue({ jobId: 1, status: 'queued' });
    const startSampleBankAssignment = vi.fn().mockResolvedValue({ jobId: 2, status: 'queued' });
    const startObjectFormatConversion = vi.fn();
    const catalog = {
        sampleBanks: banks,
        programs: [],
        relationships: [],
        membersForBank: () => [],
        selectedBankId: '',
        inspectorObjectId: '',
        editorObjectIds: { 'sample-banks': '' },
    };
    const workflow = new MutationWorkflow({
        transport: {
            startSampleBankCreation,
            startSampleBankAssignment,
            startObjectFormatConversion,
        } as unknown as ImageTransport,
        jobs: {
            run: async (start: () => Promise<unknown>) => {
                await start();
                return { status: 'completed' };
            },
        } as never,
        catalog: catalog as never,
        audition: { invalidateSession: vi.fn().mockResolvedValue(undefined) } as never,
        sessionId: () => 7,
        imageOpen: () => true,
        workspaceView: () => 'samples',
        setWorkspaceView: vi.fn(),
        clearSelection: vi.fn(),
        refreshSession: vi.fn().mockResolvedValue(undefined),
        setStatus: vi.fn(),
        reportTiming: vi.fn(),
        ...(preferredGeneration ? { preferredASeriesGeneration: () => preferredGeneration } : {}),
    });
    workflow.setCapabilities({
        volumeMutationsAvailable: true,
        partitionMutationsAvailable: true,
        objectRenameAvailable: true,
    });
    return { workflow, startSampleBankCreation, startSampleBankAssignment, startObjectFormatConversion };
}

describe('Sample Bank creation format', () => {
    it.each<{
        formats: (SampleStorageFormat | undefined)[];
        preferred?: 'A3000' | 'A4000_A5000';
        expected: Exclude<SampleStorageFormat, 'UNKNOWN'>;
    }>([
        { formats: ['A3000_188'], preferred: 'A4000_A5000', expected: 'A3000_188' },
        { formats: ['A4000_A5000_224'], expected: 'A4000_A5000_224' },
        { formats: ['A3000_188', 'A4000_A5000_224'], expected: 'A4000_A5000_224' },
        { formats: ['A4000_A5000_224', 'A3000_188'], expected: 'A4000_A5000_224' },
        { formats: ['UNKNOWN', 'A3000_188', undefined], preferred: 'A4000_A5000', expected: 'A3000_188' },
        { formats: ['UNKNOWN', 'A4000_A5000_224'], expected: 'A4000_A5000_224' },
        { formats: ['UNKNOWN', undefined], expected: 'A3000_188' },
        { formats: ['UNKNOWN', undefined], preferred: 'A4000_A5000', expected: 'A4000_A5000_224' },
    ])('chooses $expected for $formats with preference $preferred', ({ formats, preferred, expected }) => {
        const { workflow } = createWorkflow(preferred);
        const samples = formats.map((format, index) => sample(`Sample ${index}`, format));
        const before = structuredClone(samples);

        workflow.requestSampleBankAssignment(samples);

        expect(workflow.sampleBankAssignmentRequest?.initialSampleFormat).toBe(expected);
        expect(samples).toEqual(before);
    });

    it.each(['A3000_188', 'A4000_A5000_224'] as const)(
        'submits the explicit %s override without converting selected members',
        async (sampleFormat) => {
            const { workflow, startSampleBankCreation, startObjectFormatConversion } = createWorkflow();
            const samples = [sample('Native', 'A3000_188'), sample('Later', 'A4000_A5000_224')];
            const before = structuredClone(samples);
            workflow.requestSampleBankAssignment(samples);

            await workflow.submitSampleBankAssignment({ mode: 'new', name: 'Layered', sampleFormat });

            expect(startSampleBankCreation).toHaveBeenCalledExactlyOnceWith(7, {
                partitionIndex: 0,
                volumeName: 'Samples',
                sampleBankName: 'Layered',
                sampleNames: ['Native', 'Later'],
                sampleFormat,
            });
            expect(startObjectFormatConversion).not.toHaveBeenCalled();
            expect(samples).toEqual(before);
        },
    );

    it('retains existing target metadata and submits only membership assignment', async () => {
        const bank = sample('Existing Bank', 'A3000_188');
        bank.objectType = 'SBAC';
        bank.object.objectType = 'SBAC';
        bank.object.categoryName = 'SBAC';
        const { workflow, startSampleBankCreation, startSampleBankAssignment, startObjectFormatConversion } =
            createWorkflow('A4000_A5000', [bank]);
        workflow.requestSampleBankAssignment([sample('Later', 'A4000_A5000_224')]);

        expect(workflow.sampleBankAssignmentRequest?.options[0].sampleFormat).toEqual(bank.object.sampleFormat);
        await workflow.submitSampleBankAssignment({ mode: 'existing', bankObjectId: bank.objectId });

        expect(startSampleBankAssignment).toHaveBeenCalledExactlyOnceWith(7, {
            partitionIndex: 0,
            volumeName: 'Samples',
            sampleBankName: 'Existing Bank',
            sampleNames: ['Later'],
        });
        expect(startSampleBankCreation).not.toHaveBeenCalled();
        expect(startObjectFormatConversion).not.toHaveBeenCalled();
        expect(bank.object.sampleFormat).toEqual(metadata('A3000_188'));
    });
});
