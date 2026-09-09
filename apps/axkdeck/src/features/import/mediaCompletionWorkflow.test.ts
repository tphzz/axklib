import { describe, expect, it, vi } from 'vitest';
import { serverFileLocation } from '../../lib/storageLocations';
import type { ImageTransport } from '../../lib/transport';
import type { DiskTreeItem } from '../../lib/types';
import { PickerController } from '../dialogs/picker';
import { JobController } from '../jobs/actions';
import { AudioImportWorkflow } from './audioWorkflow.svelte';
import { SequenceImportWorkflow } from './sequenceWorkflow.svelte';

describe.each(['audio', 'midi'] as const)('%s completion recovery', (kind) => {
    function create() {
        const start = vi.fn().mockResolvedValue({ jobId: 41, status: 'queued' });
        const waitForJob = vi.fn().mockResolvedValue({ jobId: 41, status: 'completed' });
        const transport = {
            startAudioImport: start,
            startSequenceImport: start,
            waitForJob,
            cancelJob: vi.fn(),
        } as unknown as ImageTransport;
        const refreshSession = vi.fn().mockRejectedValueOnce(new Error('Offline')).mockResolvedValue(undefined);
        const selected: DiskTreeItem = { id: 'volume', name: 'Test', kind: 'volume', partitionIndex: 0, childCount: 0 };
        const dependencies = {
            transport,
            jobs: new JobController(transport),
            picker: new PickerController(vi.fn()),
            sessionId: () => 8,
            imageLocation: () => serverFileLocation({ rootId: 'root', relativePath: 'test.hds' }),
            imageFormat: () => 'sfs',
            mutationsAvailable: () => true,
            selectedSource: () => selected,
            setSelectedSource: vi.fn(),
            sourceItems: () => [selected],
            activeVolumeId: () => selected.id,
            samples: () => [],
            sampleBanks: () => [],
            sequences: () => [],
            loadVolume: vi.fn(),
            refreshSession,
            invalidateSession: vi.fn(),
            selectWorkspace: vi.fn(),
            selectSampleBank: vi.fn(),
            selectSample: vi.fn(),
            selectSequence: vi.fn(),
            setStatus: vi.fn(),
            reportTiming: vi.fn(),
        };
        const workflow =
            kind === 'audio' ? new AudioImportWorkflow(dependencies) : new SequenceImportWorkflow(dependencies);
        workflow.chooseFiles();
        const commit = () =>
            workflow instanceof AudioImportWorkflow
                ? workflow.commit([], { kind: 'SAMPLES' })
                : workflow.commit([], 'exclude');
        return { workflow, commit, start, waitForJob, refreshSession };
    }

    it('retains a committed import until refresh succeeds without writing twice', async () => {
        const { workflow, commit, start, refreshSession } = create();
        expect(await commit()).toBe(false);
        expect(workflow.completion.phase).toBe('refresh-failed');
        expect(workflow.request).not.toBeNull();
        expect(await commit()).toBe(false);
        expect(await workflow.completion.recover()).toBe(true);
        expect(start).toHaveBeenCalledTimes(1);
        expect(refreshSession).toHaveBeenCalledTimes(2);
    });

    it('checks the original job after losing its result', async () => {
        const { workflow, commit, start, waitForJob, refreshSession } = create();
        refreshSession.mockReset().mockResolvedValue(undefined);
        waitForJob.mockRejectedValueOnce(new Error('Disconnected'));
        expect(await commit()).toBe(false);
        expect(workflow.completion.phase).toBe('unconfirmed');
        expect(await workflow.completion.recover()).toBe(true);
        expect(start).toHaveBeenCalledTimes(1);
        expect(refreshSession).toHaveBeenCalledTimes(1);
    });
});
