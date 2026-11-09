import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import type { Tx16wImportRequest, Tx16wVolumeOption } from '../../features/import/tx16wWorkflow.svelte';
import type { ConversionPlan } from '../conversions';
import { fitsCapacity } from '../../test/samplerCapacityFixture';
import Tx16wImportDialog from './Tx16wImportDialog.svelte';
import { ImportCompletion } from '../../features/import/importCompletion.svelte';
import { JobController } from '../../features/jobs/actions';
import { idleCompanionSearch } from '../../features/import/tx16wCompanions';

const target = { partitionIndex: 1, volumeName: 'TX16W' };
const option: Tx16wVolumeOption = {
    partitionName: 'Partition 2',
    key: '1:TX16W',
    label: 'Partition 2 · TX16W',
    target,
};

function inspection(): ConversionPlan {
    return {
        planId: 'conversion-1',
        state: 'READY',
        expiresAt: Date.now() + 900000,
        jobId: null,
        report: {
            profileId: 'yamaha.tx16w-to-a-series',
            fidelity: 'BEST',
            executable: true,
            actions: [],
            issues: [],
            actionCount: 0,
            issueCount: 0,
            offset: 0,
            capacity: fitsCapacity,
            destination: { imageId: 'image-1', documentId: 'doc-1', expectedRevision: 1 },
        },
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
                origin: 'SELECTED',
                sourceIdentity: 'fixture:library',
            },
        ],
        target,
        importMode: 'HIERARCHY',
        plan: inspection(),
        fidelity: 'BEST',
        acknowledgedIssueIds: [],
        discovery: idleCompanionSearch(),
        status: 'ready',
        progress: 1,
        error: '',
    };
}

describe('Tx16wImportDialog', () => {
    it.each(['discovering', 'inspecting', 'uploading'] as const)('keeps Cancel available while %s', async (status) => {
        const current = request();
        current.status = status;
        const oncancel = vi.fn();
        render(Tx16wImportDialog, {
            request: current,
            completion: new ImportCompletion(
                { waitForJob: vi.fn() },
                new JobController({ waitForJob: vi.fn(), cancelJob: vi.fn() }),
            ),
            volumeOptions: [option],
            oncancel,
            onrecover: vi.fn(),
            ontarget: vi.fn(),
            onmode: vi.fn(),
            onfidelity: vi.fn(),
            onprepare: vi.fn(),
            onadd: vi.fn(),
            onremove: vi.fn(),
            onconfirm: vi.fn(),
        });
        expect((screen.getByRole('button', { name: 'Import' }) as HTMLButtonElement).disabled).toBe(true);
        await fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));
        expect(oncancel).toHaveBeenCalledOnce();
    });

    it('exposes search failure Retry and blocks Import even with a clean conversion report', async () => {
        const current = request();
        current.discovery = {
            state: 'error',
            error: 'Scan limit exceeded',
            message: 'Companion search incomplete.',
            requirements: [],
        };
        const onprepare = vi.fn();
        render(Tx16wImportDialog, {
            request: current,
            completion: new ImportCompletion(
                { waitForJob: vi.fn() },
                new JobController({ waitForJob: vi.fn(), cancelJob: vi.fn() }),
            ),
            volumeOptions: [option],
            oncancel: vi.fn(),
            onrecover: vi.fn(),
            ontarget: vi.fn(),
            onmode: vi.fn(),
            onfidelity: vi.fn(),
            onprepare,
            onadd: vi.fn(),
            onremove: vi.fn(),
            onconfirm: vi.fn(),
        });
        expect((screen.getByRole('button', { name: 'Import' }) as HTMLButtonElement).disabled).toBe(true);
        expect((screen.getByLabelText('Add disks') as HTMLInputElement).disabled).toBe(false);
        await fireEvent.click(screen.getByRole('button', { name: 'Retry' }));
        expect(onprepare).toHaveBeenCalledOnce();
    });

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
                request: { ...request(), status: 'importing' },
                volumeOptions: [option],
                oncancel,
                onrecover: vi.fn(),
                ontarget: vi.fn(),
                onmode: vi.fn(),
                onfidelity: vi.fn(),
                onprepare: vi.fn(),
                onadd: vi.fn(),
                onremove: vi.fn(),
                onconfirm: vi.fn(),
            },
        });
        const fileInput = screen.getByLabelText('Add disks') as HTMLInputElement;
        expect(fileInput.disabled).toBe(true);
        expect(fileInput.closest('label')?.getAttribute('aria-disabled')).toBe('true');
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
                onfidelity: vi.fn(),
                onprepare: vi.fn(),
                onadd: vi.fn(),
                onremove: vi.fn(),
                onconfirm,
                oncancel: vi.fn(),
            },
        });

        const importButton = screen.getByRole('button', { name: 'Import' });
        const fileInput = screen.getByLabelText('Add disks') as HTMLInputElement;
        expect(fileInput.disabled).toBe(false);
        expect(fileInput.closest('label')?.getAttribute('aria-disabled')).toBe('false');
        expect((screen.getByRole('button', { name: 'New' }) as HTMLButtonElement).disabled).toBe(true);
        await fireEvent.focus(screen.getByRole('button', { name: 'Destination availability' }));
        expect(screen.getByRole('tooltip').textContent).toBe('TX16W conversion requires an existing volume');
        expect(screen.queryByRole('button', { name: 'Review' })).toBeNull();
        expect((importButton as HTMLButtonElement).disabled).toBe(false);

        await fireEvent.input(screen.getByRole('combobox', { name: 'Destination volume' }), {
            target: { value: 'Partition 2 · Missing' },
        });

        expect((importButton as HTMLButtonElement).disabled).toBe(true);
        expect(onconfirm).not.toHaveBeenCalled();
    });
});
