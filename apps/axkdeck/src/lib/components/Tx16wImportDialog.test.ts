import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import type { Tx16wImportRequest } from '../../features/import/tx16wWorkflow.svelte';
import type { ConversionPlan } from '../conversions';
import { fitsCapacity } from '../../test/samplerCapacityFixture';
import Tx16wImportDialog from './Tx16wImportDialog.svelte';
import { ImportCompletion } from '../../features/import/importCompletion.svelte';
import { JobController } from '../../features/jobs/actions';
import { idleCompanionSearch } from '../../features/import/tx16wCompanions';

const target = { kind: 'EXISTING_VOLUME' as const, partitionIndex: 1, volumeName: 'TX16W' };
const destinations = {
    partitions: [
        { partitionIndex: 1, name: 'Partition 2' },
        { partitionIndex: 2, name: 'Empty partition' },
    ],
    volumes: [{ ...target, name: 'Partition 2', label: 'Partition 2 · TX16W' }],
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
    it('clears a pending partition on a different destination and displays the current inspected placement', async () => {
        const current: Tx16wImportRequest = {
            ...request(),
            documentSource: { kind: 'DOCUMENT', documentId: 'source', sessionId: 7, expectedRevision: 1 },
            destinationName: 'first.hds',
            target: null,
            members: [],
        };
        const ontarget = vi.fn();
        const view = render(Tx16wImportDialog, {
            request: current,
            destinations,
            completion: new ImportCompletion(
                { waitForJob: vi.fn() },
                new JobController({ waitForJob: vi.fn(), cancelJob: vi.fn() }),
            ),
            onrecover: vi.fn(),
            ontarget,
            onmode: vi.fn(),
            onfidelity: vi.fn(),
            onprepare: vi.fn(),
            onadd: vi.fn(),
            onremove: vi.fn(),
            onconfirm: vi.fn(),
            oncancel: vi.fn(),
        });
        const partition = () => view.getByRole('combobox', { name: 'Destination partition' });
        const volume = () => view.getByRole('combobox', { name: 'Destination volume' }) as HTMLInputElement;
        await fireEvent.click(partition());
        await fireEvent.click(screen.getByRole('option', { name: 'Partition 2' }));
        expect(ontarget).toHaveBeenCalledWith(target);
        await view.rerender({ request: { ...current, target } });
        expect(partition().textContent).toContain('Partition 2');
        expect(volume().disabled).toBe(false);
        const replacement = { ...current, destinationName: 'second.hds' };
        await view.rerender({ request: replacement });
        expect(partition().textContent).toContain('Select partition');
        expect(volume().disabled).toBe(true);
        expect(view.getByRole('button', { name: 'Add to volume' }).hasAttribute('disabled')).toBe(true);
        await view.rerender({ request: { ...replacement, target } });
        expect(partition().textContent).toContain('Partition 2');
        expect(volume().value).toBe(target.volumeName);
    });
    it('allows immediate import for known mapping differences and requests only a real mapping decision', async () => {
        const current = request();
        const report = current.plan!.report!;
        report.issues = [
            {
                id: 'loss',
                code: 'sample-rate',
                impact: 'LOSS',
                message: 'Sample rate converted',
                sourceObject: 'Wave',
                sourceParameter: 'rate',
                targetObject: '',
                targetParameter: '',
            },
            {
                id: 'default',
                code: 'defaults',
                impact: 'UNCERTAINTY',
                message: 'Using writer defaults',
                sourceObject: '',
                sourceParameter: '',
                targetObject: '',
                targetParameter: '',
            },
        ];
        report.issueCount = report.issues.length;
        const onconfirm = vi.fn();
        const view = render(Tx16wImportDialog, {
            request: current,
            completion: new ImportCompletion(
                { waitForJob: vi.fn() },
                new JobController({ waitForJob: vi.fn(), cancelJob: vi.fn() }),
            ),
            destinations,
            onconfirm,
            oncancel: vi.fn(),
            onrecover: vi.fn(),
            ontarget: vi.fn(),
            onmode: vi.fn(),
            onfidelity: vi.fn(),
            onprepare: vi.fn(),
            onadd: vi.fn(),
            onremove: vi.fn(),
        });
        const primary = () => view.getByRole('button', { name: /^Import$/ }) as HTMLButtonElement;
        expect(primary().disabled).toBe(false);
        expect(current.acknowledgedIssueIds).toEqual([]);
        await fireEvent.click(primary());
        expect(onconfirm).toHaveBeenCalledOnce();
        const next = {
            ...current,
            plan: {
                ...current.plan!,
                report: {
                    ...report,
                    issues: [
                        ...report.issues,
                        {
                            ...report.issues[1],
                            id: 'audition',
                            decision: 'Create an audition Program from Piano Voice',
                        },
                    ],
                    issueCount: 3,
                },
            },
        };
        await view.rerender({ request: next });
        expect(primary().disabled).toBe(true);
        expect(view.getByRole('checkbox', { name: 'Create an audition Program from Piano Voice' })).toBeTruthy();
    });

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
            destinations,
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
            destinations,
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
                destinations,
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
        const ontarget = vi.fn();
        const view = render(Tx16wImportDialog, {
            props: {
                completion: new ImportCompletion(
                    { waitForJob: vi.fn() },
                    new JobController({ waitForJob: vi.fn(), cancelJob: vi.fn() }),
                ),
                onrecover: vi.fn(),
                request: request(),
                destinations,
                ontarget,
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
        expect((screen.getByRole('button', { name: 'New' }) as HTMLButtonElement).disabled).toBe(false);
        expect(screen.queryByRole('button', { name: 'Review' })).toBeNull();
        expect((importButton as HTMLButtonElement).disabled).toBe(false);

        await fireEvent.input(screen.getByRole('combobox', { name: 'Destination volume' }), {
            target: { value: 'Partition 2 · Missing' },
        });
        expect(ontarget).toHaveBeenLastCalledWith({ ...target, volumeName: '' });
        await view.rerender({ request: { ...request(), target: { ...target, volumeName: '' } } });
        expect((importButton as HTMLButtonElement).disabled).toBe(true);
        await fireEvent.click(screen.getByRole('button', { name: 'New' }));
        expect(ontarget).toHaveBeenLastCalledWith({ kind: 'CREATE_VOLUME', partitionIndex: 1, volumeName: 'library' });
        await view.rerender({ request: { ...request(), target: { ...target, kind: 'CREATE_VOLUME' } } });
        const name = screen.getByRole('textbox', { name: 'New volume name' });
        expect(name.getAttribute('aria-invalid')).toBe('true');
        expect(document.getElementById(name.getAttribute('aria-describedby')!)?.textContent).toMatch(/already exists/);
        expect(screen.getByRole('button', { name: 'Create volume and import' }).hasAttribute('disabled')).toBe(true);
        await fireEvent.click(screen.getByRole('combobox', { name: 'Destination partition' }));
        await fireEvent.click(screen.getByRole('option', { name: 'Empty partition' }));
        expect(ontarget).toHaveBeenLastCalledWith({ kind: 'CREATE_VOLUME', partitionIndex: 2, volumeName: 'library' });
        expect(onconfirm).not.toHaveBeenCalled();
    });
    it('returns to the explicitly selected existing volume after testing a new name', async () => {
        const ontarget = vi.fn();
        const view = render(Tx16wImportDialog, {
            request: request(),
            completion: new ImportCompletion(
                { waitForJob: vi.fn() },
                new JobController({ waitForJob: vi.fn(), cancelJob: vi.fn() }),
            ),
            destinations: {
                ...destinations,
                volumes: [{ ...destinations.volumes[0], volumeName: 'First', label: 'First' }, ...destinations.volumes],
            },
            ontarget,
            onrecover: vi.fn(),
            onmode: vi.fn(),
            onfidelity: vi.fn(),
            onprepare: vi.fn(),
            onadd: vi.fn(),
            onremove: vi.fn(),
            onconfirm: vi.fn(),
            oncancel: vi.fn(),
        });
        await fireEvent.click(screen.getByRole('button', { name: 'New' }));
        await view.rerender({
            request: { ...request(), target: { ...target, kind: 'CREATE_VOLUME', volumeName: 'New volume' } },
        });
        await fireEvent.click(screen.getByRole('button', { name: 'Existing' }));
        expect(ontarget).toHaveBeenLastCalledWith(target);
    });
});
