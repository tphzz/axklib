import { describe, expect, it, vi } from 'vitest';
import { serverFileLocation } from '../../lib/storageLocations';
import type { ImageTransport } from '../../lib/transport';
import type { DiskTreeItem } from '../../lib/types';
import { PickerController } from '../dialogs/picker';
import type { JobController } from '../jobs/actions';
import { AudioImportWorkflow } from './audioWorkflow.svelte';

describe('AudioImportWorkflow', () => {
    it('clears an existing volume without loading another volume when its partition changes', () => {
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
        const loadVolume = vi.fn();
        const workflow = new AudioImportWorkflow({
            transport: {} as ImageTransport,
            jobs: {} as JobController,
            picker: new PickerController(() => undefined),
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
            refreshSession: vi.fn(),
            invalidateSession: vi.fn(),
            selectWorkspace: vi.fn(),
            selectSampleBank: vi.fn(),
            selectSample: vi.fn(),
            setStatus: vi.fn(),
            reportTiming: vi.fn(),
        });

        workflow.chooseFiles();
        workflow.setDestinationPartition(1);

        expect(workflow.request).toMatchObject({
            destinationMode: 'existing',
            destinationPartitionIndex: 1,
            destinationVolumeName: '',
        });
        expect(loadVolume).not.toHaveBeenCalled();
    });
});
