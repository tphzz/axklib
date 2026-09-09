import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import type { Tx16wImportRequest, Tx16wVolumeOption } from '../../features/import/tx16wWorkflow.svelte';
import type { Tx16wImportInspection } from '../transport';
import Tx16wImportDialog from './Tx16wImportDialog.svelte';
import { ImportCompletion } from '../../features/import/importCompletion.svelte';
import { JobController } from '../../features/jobs/actions';

const target = { partitionIndex: 1, volumeName: 'TX16W' };
const option: Tx16wVolumeOption = {
    partitionName: 'Partition 2',
    key: '1:TX16W',
    label: 'Partition 2 · TX16W',
    target,
};

function inspection(): Tx16wImportInspection {
    return {
        schemaVersion: '1.0',
        sourceMembers: ['library.ima'],
        importMode: 'HIERARCHY',
        profile: 'YAMAHA_NATIVE',
        target,
        valid: true,
        counts: { programs: 1, sampleBanks: 1, samples: 1, waveData: 1 },
        objects: { programs: [], sampleBanks: [], samples: [], waveData: [] },
        notices: [],
    };
}

function request(): Tx16wImportRequest {
    return {
        members: [
            {
                id: 1,
                source: {
                    name: 'library.ima',
                    type: 'application/octet-stream',
                    size: 1_474_560,
                    readChunk: vi.fn(),
                },
                sourceName: 'library.ima',
                resolvedSource: null,
                upload: null,
            },
        ],
        target,
        importMode: 'HIERARCHY',
        inspection: inspection(),
        status: 'ready',
        progress: 1,
        error: '',
    };
}

describe('Tx16wImportDialog', () => {
    it('locks source and destination edits while retaining completion warnings', async () => {
        const completion = new ImportCompletion(
            { waitForJob: vi.fn() },
            new JobController({ waitForJob: vi.fn(), cancelJob: vi.fn() }),
        );
        completion.phase = 'warnings';
        completion.warnings = ['Check the imported program'];
        const oncancel = vi.fn();
        render(Tx16wImportDialog, {
            props: {
                completion,
                request: request(),
                volumeOptions: [option],
                oncancel,
                onrecover: vi.fn(),
                ontarget: vi.fn(),
                onmode: vi.fn(),
                onadd: vi.fn(),
                onremove: vi.fn(),
                onconfirm: vi.fn(),
            },
        });
        expect((screen.getByLabelText('Add disks') as HTMLInputElement).disabled).toBe(true);
        expect((screen.getByRole('combobox', { name: 'Destination volume' }) as HTMLInputElement).disabled).toBe(true);
        expect(screen.queryByRole('button', { name: 'Import' })).toBeNull();
        await fireEvent.click(screen.getByRole('button', { name: 'Done' }));
        expect(oncancel).toHaveBeenCalledOnce();
    });
    it('requires the visible target label to match the inspected target', async () => {
        const onconfirm = vi.fn();
        render(Tx16wImportDialog, {
            props: {
                completion: new ImportCompletion(
                    { waitForJob: vi.fn() },
                    new JobController({ waitForJob: vi.fn(), cancelJob: vi.fn() }),
                ),
                onrecover: vi.fn(),
                request: request(),
                volumeOptions: [option],
                ontarget: vi.fn(),
                onmode: vi.fn(),
                onadd: vi.fn(),
                onremove: vi.fn(),
                onconfirm,
                oncancel: vi.fn(),
            },
        });

        const importButton = screen.getByRole('button', { name: 'Import' });
        expect((screen.getByRole('button', { name: 'New' }) as HTMLButtonElement).disabled).toBe(true);
        expect(screen.queryByRole('button', { name: 'Review' })).toBeNull();
        expect((importButton as HTMLButtonElement).disabled).toBe(false);

        await fireEvent.input(screen.getByRole('combobox', { name: 'Destination volume' }), {
            target: { value: 'Partition 2 · Missing' },
        });

        expect((importButton as HTMLButtonElement).disabled).toBe(true);
        expect(onconfirm).not.toHaveBeenCalled();
    });
});
