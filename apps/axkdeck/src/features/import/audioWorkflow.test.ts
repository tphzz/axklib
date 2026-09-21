import { describe, expect, it, vi } from 'vitest';
import { ASeriesPreferences, type ASeriesGeneration } from '../../lib/aSeriesPreferences.svelte';
import { serverFileLocation } from '../../lib/storageLocations';
import type { ImageTransport } from '../../lib/transport';
import type { DiskTreeItem } from '../../lib/types';
import { PickerController } from '../dialogs/picker';
import { JobController } from '../jobs/actions';
import { AudioImportWorkflow } from './audioWorkflow.svelte';

const firstVolume: DiskTreeItem = {
    id: 'volume-0',
    name: 'First',
    kind: 'volume',
    childCount: 0,
    partitionIndex: 0,
};
const secondVolume: DiskTreeItem = {
    id: 'volume-1',
    name: 'Second',
    kind: 'volume',
    childCount: 0,
    partitionIndex: 1,
};
const audioFile = serverFileLocation({ rootId: 'root', relativePath: 'Tone.wav' });

function createWorkflow(preferredASeriesGeneration?: () => ASeriesGeneration) {
    const loadVolume = vi.fn().mockResolvedValue(undefined);
    const startAudioImport = vi.fn().mockResolvedValue({ jobId: 41, status: 'queued' });
    const waitForJob = vi.fn().mockResolvedValue({ jobId: 41, status: 'completed' });
    const transport = { startAudioImport, waitForJob, cancelJob: vi.fn() } as unknown as ImageTransport;
    const picker = new PickerController(() => undefined);
    const workflow = new AudioImportWorkflow({
        transport,
        jobs: new JobController(transport),
        picker,
        sessionId: () => 17,
        imageLocation: () => serverFileLocation({ rootId: 'root', relativePath: 'image.hds' }),
        imageFormat: () => 'sfs',
        mutationsAvailable: () => true,
        selectedSource: () => firstVolume,
        setSelectedSource: vi.fn(),
        sourceItems: () => [firstVolume, secondVolume],
        activeVolumeId: () => firstVolume.id,
        sampleBanks: () => [],
        samples: () => [],
        loadVolume,
        refreshSession: vi.fn().mockResolvedValue(undefined),
        invalidateSession: vi.fn().mockResolvedValue(undefined),
        selectWorkspace: vi.fn(),
        selectSampleBank: vi.fn(),
        selectSample: vi.fn(),
        setStatus: vi.fn(),
        reportTiming: vi.fn(),
        ...(preferredASeriesGeneration ? { preferredASeriesGeneration } : {}),
    });
    return { workflow, picker, loadVolume, startAudioImport, waitForJob };
}

describe('AudioImportWorkflow', () => {
    it('clears an existing volume without loading another volume when its partition changes', () => {
        const { workflow, loadVolume } = createWorkflow();
        workflow.chooseFiles();
        workflow.sampleFormat = 'A4000_A5000_224';
        workflow.setDestinationPartition(1);

        expect(workflow.request).toMatchObject({
            destinationMode: 'existing',
            destinationPartitionIndex: 1,
            destinationVolumeName: '',
        });
        expect(workflow.sampleFormat).toBe('A4000_A5000_224');
        expect(loadVolume).not.toHaveBeenCalled();
    });

    it.each(['choose', 'drop'] as const)(
        'resets each new %s request to the currently saved preference',
        async (mode) => {
            let generation: ASeriesGeneration = 'A4000_A5000';
            const { workflow } = createWorkflow(() => generation);
            const open = async () => {
                if (mode === 'choose') workflow.chooseFiles();
                else await workflow.requestDroppedFiles([audioFile]);
            };
            await open();
            expect(workflow.sampleFormat).toBe('A4000_A5000_224');
            workflow.sampleFormat = 'A3000_188';
            workflow.request = null;
            await open();
            expect(workflow.sampleFormat).toBe('A4000_A5000_224');

            generation = 'A3000';
            workflow.request = null;
            await open();
            expect(workflow.sampleFormat).toBe('A3000_188');
        },
    );

    it('uses A3k when no preference provider is installed', () => {
        const { workflow } = createWorkflow();
        workflow.chooseFiles();
        expect(workflow.sampleFormat).toBe('A3000_188');
    });

    it('retains a manual format while browsing files and retrying a confirmed failure', async () => {
        const { workflow, picker, startAudioImport, waitForJob } = createWorkflow(() => 'A3000');
        workflow.chooseFiles();
        workflow.sampleFormat = 'A4000_A5000_224';
        const choose = vi.spyOn(picker, 'chooseFiles').mockResolvedValueOnce(null).mockResolvedValueOnce([audioFile]);

        await workflow.chooseWorkspace();
        expect(workflow.sampleFormat).toBe('A4000_A5000_224');
        await workflow.chooseWorkspace();
        expect(choose).toHaveBeenCalledTimes(2);
        expect(workflow.request?.files).toEqual([audioFile]);
        expect(workflow.sampleFormat).toBe('A4000_A5000_224');

        waitForJob.mockResolvedValueOnce({ jobId: 41, status: 'failed', error: 'Name conflict' });
        const commit = () =>
            workflow.commit([], { sampleFormat: workflow.sampleFormat, grouping: { kind: 'SAMPLES' } });
        expect(await commit()).toBe(false);
        expect(workflow.completion.phase).toBe('idle');
        expect(workflow.sampleFormat).toBe('A4000_A5000_224');
        expect(await commit()).toBe(true);
        expect(startAudioImport).toHaveBeenCalledTimes(2);
        expect(startAudioImport.mock.calls.map((call) => call[3])).toEqual([
            { sampleFormat: 'A4000_A5000_224', grouping: { kind: 'SAMPLES' } },
            { sampleFormat: 'A4000_A5000_224', grouping: { kind: 'SAMPLES' } },
        ]);
    });

    it('does not save a per-request override when the import is cancelled', async () => {
        const save = vi.fn().mockResolvedValue(undefined);
        const preferences = new ASeriesPreferences({ load: async () => 'A3000', save });
        await preferences.ready;
        const { workflow } = createWorkflow(() => preferences.generation);
        workflow.chooseFiles();
        workflow.sampleFormat = 'A4000_A5000_224';

        workflow.request = null;

        expect(preferences.generation).toBe('A3000');
        expect(save).not.toHaveBeenCalled();
        workflow.chooseFiles();
        expect(workflow.sampleFormat).toBe('A3000_188');
    });
});
