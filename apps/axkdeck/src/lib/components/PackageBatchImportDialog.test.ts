import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { serverFileLocation } from '../storageLocations';
import type { ImageSessionPackageImportPlan, PackageInspection } from '../transport';
import type { BatchPackageItem } from '../../features/import/packageBatchTypes';
import PackageBatchImportDialog from './PackageBatchImportDialog.svelte';

const dialogSource = readFileSync(resolve(process.cwd(), 'src/lib/components/PackageBatchImportDialog.svelte'), 'utf8');
const tableSource = readFileSync(resolve(process.cwd(), 'src/lib/components/PackageBatchItemsTable.svelte'), 'utf8');

function inspection(packageId: string, volumeName: string): PackageInspection {
    return {
        schemaVersion: '1.0',
        packageId,
        packageKind: 'VOLUME',
        requiredExtension: '.axkvol',
        sourceMediaKind: 'SFS',
        valid: true,
        payloadsVerified: true,
        totalPayloadBytes: 4096,
        roots: [{ kind: 'VOLUME', displayName: volumeName, nodeIds: [] }],
        objects: [],
        relationships: [],
        relationshipCount: 0,
        issues: [],
    };
}

const items: BatchPackageItem[] = ['one', 'two'].map((stem, packageIndex) => ({
    id: `item-${packageIndex}`,
    selected: true,
    source: serverFileLocation({ rootId: 'workspace', relativePath: `${stem}.axkvol` }, `${stem}.axkvol`),
    sourceName: `${stem}.axkvol`,
    inspection: inspection(`package-${packageIndex}`, 'Drums'),
    upload: null,
    localPath: null,
}));

const plan: ImageSessionPackageImportPlan = {
    schemaVersion: '1.0',
    imageId: 'image-1',
    revision: 1,
    planToken: 'plan-1',
    expiresInSeconds: 600,
    planId: 'plan-id',
    targetKind: 'SFS',
    targetSnapshotId: 'snapshot-1',
    packages: ['Drums', 'Drums 2'].map((destinationVolumeName, packageIndex) => ({
        packageIndex,
        packageId: `package-${packageIndex}`,
        sourceVolumeName: 'Drums',
        destinationVolumeName,
        objectCount: 15 + packageIndex,
        payloadBytes: 4096,
        objectCounts: {
            programs: 1,
            sampleBanks: 2,
            samples: 3,
            waveData: 8,
            sequences: 1,
        },
    })),
    valid: true,
    warnings: [],
    conflicts: [],
    actions: [],
    opaqueSequences: [],
    programAssignmentAdjustments: [],
    programSlotPlacements: [],
    allocation: [],
    sfsIndexCapacity: [
        {
            partitionIndex: 0,
            indexBlockCount: 358,
            recordsPerIndexBlock: 14,
            totalRecordSlots: 5012,
            reservedRecordSlots: 3,
            allocatableRecordSlots: 5009,
            usedRecordSlots: 4310,
            freeRecordSlots: 699,
            requiredRecordSlots: 37,
            allocatedRecordSlots: 37,
            shortfallRecordSlots: 0,
            remainingRecordSlots: 662,
            packages: [
                {
                    packageIndex: 0,
                    effectiveObjectRecordSlots: 15,
                    volumeScaffoldingRecordSlots: 6,
                    standaloneRequiredRecordSlots: 21,
                    plannedObjectRecordSlots: 15,
                    plannedRecordSlots: 21,
                    reusedObjectCount: 0,
                    allocatedRecordSlots: 21,
                    shortfallRecordSlots: 0,
                },
                {
                    packageIndex: 1,
                    effectiveObjectRecordSlots: 16,
                    volumeScaffoldingRecordSlots: 0,
                    standaloneRequiredRecordSlots: 16,
                    plannedObjectRecordSlots: 16,
                    plannedRecordSlots: 16,
                    reusedObjectCount: 0,
                    allocatedRecordSlots: 16,
                    shortfallRecordSlots: 0,
                },
            ],
        },
    ],
};

const callbacks = {
    onchooseworkspace: vi.fn(),
    onchooselocal: vi.fn(),
    ondestinationstrategy: vi.fn(),
    ondestinationmode: vi.fn(),
    ondestinationvolume: vi.fn(),
    ondestinationpartition: vi.fn(),
    ondestinationname: vi.fn(),
    onrenamevolume: vi.fn(),
    onrename: vi.fn(),
    onprogramslot: vi.fn(),
    onprogramstart: vi.fn(),
    ontoggleselected: vi.fn(),
    ontoggleall: vi.fn(),
    onopaquesequenceaction: vi.fn(),
    onreplan: vi.fn(),
    oncancel: vi.fn(),
    onconfirm: vi.fn(),
};

function props() {
    return {
        desktop: false,
        canChangeSources: true,
        items,
        plan,
        destinationStrategy: 'separate' as const,
        destinationMode: 'create' as const,
        destinationPartitionIndex: 0,
        destinationVolumeName: '',
        partitionOptions: [{ partitionIndex: 0, name: 'Partition 1' }],
        volumeOptions: [
            {
                partitionIndex: 0,
                name: 'Partition 1',
                volumeName: 'Existing',
                label: 'Partition 1 / Existing',
            },
        ],
        separateVolumesAvailable: true,
        volumeNames: { 'item-0': 'Drums', 'item-1': 'Drums 2' },
        renames: {},
        programSlots: {},
        opaqueSequenceActions: {},
        hasUnvalidatedChanges: false,
        status: 'ready' as const,
        completedFiles: 2,
        totalFiles: 2,
        progress: 0,
        error: '',
        ...callbacks,
    };
}

describe('PackageBatchImportDialog', () => {
    beforeEach(() => vi.clearAllMocks());

    it('previews every destination and its object counts before one batch import', async () => {
        render(PackageBatchImportDialog, { props: props() });

        expect(screen.getByRole('heading', { name: 'Import packages' })).toBeTruthy();
        expect(screen.getByText('2 of 2 packages selected')).toBeTruthy();
        expect(screen.getByText(/2 volumes will be created/)).toBeTruthy();
        expect(screen.getAllByText('1 Programs')).toHaveLength(2);
        expect(screen.getAllByText('2 Sample Banks')).toHaveLength(2);
        expect(screen.getAllByText('3 Samples')).toHaveLength(2);
        expect(screen.getAllByText('8 Wave Data')).toHaveLength(2);
        expect(screen.getAllByText('1 Sequences')).toHaveLength(2);
        expect(screen.getByText('SFS record capacity')).toBeTruthy();
        expect(screen.getByText('699 free')).toBeTruthy();
        expect(screen.getByText('37 required')).toBeTruthy();
        expect(screen.getByText('662 remaining')).toBeTruthy();
        expect(screen.getByRole('columnheader', { name: 'SFS records' })).toBeTruthy();
        expect(screen.getByText('21')).toBeTruthy();
        expect(screen.getByText('16')).toBeTruthy();
        expect(screen.getByRole('button', { name: 'Import 2 packages' }).hasAttribute('disabled')).toBe(false);

        await fireEvent.input(screen.getByLabelText('New volume name for two.axkvol'), {
            target: { value: 'Percussion' },
        });
        expect(callbacks.onrenamevolume).toHaveBeenCalledWith('item-1', 'Percussion');
    });

    it('uses the shared destination chooser for mixed package batches', async () => {
        render(PackageBatchImportDialog, {
            props: {
                ...props(),
                destinationStrategy: 'shared',
                destinationMode: 'existing',
                destinationVolumeName: 'Existing',
                separateVolumesAvailable: false,
            },
        });

        expect(screen.getByText(/2 packages will be imported into one volume/)).toBeTruthy();
        expect(screen.getByRole('button', { name: 'One volume' }).getAttribute('aria-pressed')).toBe('true');
        expect(screen.getByRole('button', { name: 'Separate volumes' }).hasAttribute('disabled')).toBe(true);
        expect((screen.getByRole('combobox', { name: 'Volume' }) as HTMLInputElement).value).toBe(
            'Partition 1 / Existing',
        );

        await fireEvent.click(screen.getByRole('button', { name: 'New volume' }));
        expect(callbacks.ondestinationmode).toHaveBeenCalledWith('create');
    });

    it('changes native package selections with the native picker', async () => {
        render(PackageBatchImportDialog, {
            props: {
                ...props(),
                items: items.map((item) => ({ ...item, localPath: `/packages/${item.sourceName}` })),
            },
        });

        await fireEvent.click(screen.getByRole('button', { name: 'Change files' }));

        expect(callbacks.onchooselocal).toHaveBeenCalledOnce();
        expect(callbacks.onchooseworkspace).not.toHaveBeenCalled();
    });

    it('keeps unchecked packages visible and exposes an indeterminate select-all control', async () => {
        const partiallySelected = items.map((item, index) => ({ ...item, selected: index === 0 }));
        render(PackageBatchImportDialog, {
            props: { ...props(), items: partiallySelected, hasUnvalidatedChanges: true },
        });

        const selectAll = screen.getByRole('checkbox', { name: 'Select all packages' }) as HTMLInputElement;
        await vi.waitFor(() => expect(selectAll.indeterminate).toBe(true));
        expect((screen.getByRole('checkbox', { name: 'Include one.axkvol' }) as HTMLInputElement).checked).toBe(true);
        expect((screen.getByRole('checkbox', { name: 'Include two.axkvol' }) as HTMLInputElement).checked).toBe(false);
        expect(screen.getByText('1 of 2 packages selected')).toBeTruthy();
        expect(screen.getByText('Not included')).toBeTruthy();
        expect(screen.queryByText('SFS record capacity')).toBeNull();
        expect(screen.getByText('Selection or volume names changed. Check conflicts to recalculate.')).toBeTruthy();

        await fireEvent.click(screen.getByRole('checkbox', { name: 'Include two.axkvol' }));
        expect(callbacks.ontoggleselected).toHaveBeenCalledWith('item-1', true);
        await fireEvent.click(selectAll);
        expect(callbacks.ontoggleall).toHaveBeenCalledWith(true);
    });

    it('requires at least one package before conflicts can be checked', () => {
        render(PackageBatchImportDialog, {
            props: {
                ...props(),
                items: items.map((item) => ({ ...item, selected: false })),
                hasUnvalidatedChanges: true,
            },
        });

        expect(screen.getByText('Select at least one package.')).toBeTruthy();
        expect(screen.getByRole('button', { name: 'Check conflicts' }).hasAttribute('disabled')).toBe(true);
        expect(screen.getByRole('button', { name: 'Import 0 packages' }).hasAttribute('disabled')).toBe(true);
    });

    it('requires edited destinations to be checked before import', () => {
        render(PackageBatchImportDialog, {
            props: {
                ...props(),
                volumeNames: { 'item-0': 'Drums', 'item-1': 'Percussion' },
                hasUnvalidatedChanges: true,
            },
        });

        expect(screen.getByRole('button', { name: 'Check conflicts' })).toBeTruthy();
        expect(screen.getByRole('button', { name: 'Import 2 packages' }).hasAttribute('disabled')).toBe(true);
    });

    it('keeps conflict checking in the fixed footer and uses compact top-aligned rows', () => {
        render(PackageBatchImportDialog, { props: props() });

        const checkConflicts = screen.getByRole('button', { name: 'Check conflicts' });
        const footer = checkConflicts.closest('footer');
        expect(footer?.classList.contains('dialog-footer')).toBe(true);
        expect(
            Array.from(footer?.querySelectorAll('button') ?? []).map((button) => button.textContent?.trim()),
        ).toEqual(['Check conflicts', 'Cancel', 'Import 2 packages']);
        expect(footer?.querySelector('.batch-package-footer-actions')).toBeTruthy();
        expect(screen.getByText('21').tagName).toBe('SPAN');
        expect(tableSource).toMatch(/\.batch-table-row\s*\{[^}]*align-items:\s*start;/s);
        expect(tableSource).toMatch(/\.batch-table-row\s*\{[^}]*padding-block:\s*6px;/s);
        expect(dialogSource).toMatch(/\.batch-package-footer\s*\{[^}]*flex-wrap:\s*wrap;/s);
    });

    it('disables conflict checking and reports progress while planning', () => {
        render(PackageBatchImportDialog, {
            props: { ...props(), status: 'planning' },
        });

        const checking = screen.getByRole('button', { name: 'Checking conflicts…' });
        expect(checking.hasAttribute('disabled')).toBe(true);
        expect(screen.getByRole('status').textContent).toContain('Planning batch import…');
    });

    it('scrolls to the capacity summary after a successful conflict check', async () => {
        let completeCheck: (() => void) | undefined;
        callbacks.onreplan.mockReturnValueOnce(
            new Promise<void>((resolveCheck) => {
                completeCheck = resolveCheck;
            }),
        );
        render(PackageBatchImportDialog, {
            props: { ...props(), hasUnvalidatedChanges: true },
        });
        const content = screen.getByRole('dialog').querySelector<HTMLElement>('.batch-package-content');
        expect(content).toBeTruthy();
        content!.scrollTop = 480;

        await fireEvent.click(screen.getByRole('button', { name: 'Check conflicts' }));
        expect(content!.scrollTop).toBe(480);
        completeCheck?.();

        await waitFor(() => expect(content!.scrollTop).toBe(0));
    });

    it('keeps the current scroll position when conflict checking reports an error', async () => {
        const initialProps = { ...props(), hasUnvalidatedChanges: true };
        const rendered = render(PackageBatchImportDialog, { props: initialProps });
        callbacks.onreplan.mockImplementationOnce(async () => {
            await rendered.rerender({ ...initialProps, error: 'Conflict planning failed' });
        });
        const content = screen.getByRole('dialog').querySelector<HTMLElement>('.batch-package-content');
        expect(content).toBeTruthy();
        content!.scrollTop = 480;

        await fireEvent.click(screen.getByRole('button', { name: 'Check conflicts' }));

        await waitFor(() => expect(screen.getByRole('alert').textContent).toContain('Conflict planning failed'));
        expect(content!.scrollTop).toBe(480);
    });

    it('does not report readiness while an apply error is visible', () => {
        render(PackageBatchImportDialog, {
            props: { ...props(), error: 'Alteration journal storage is full' },
        });

        expect(screen.queryByText('Ready to import')).toBeNull();
        expect(screen.getByRole('alert').textContent).toContain('Alteration journal storage is full');
        expect(screen.getByRole('button', { name: 'Import 2 packages' }).hasAttribute('disabled')).toBe(true);
    });

    it('summarizes record exhaustion without repeating every blocked object', () => {
        const constrainedPlan = structuredClone(plan);
        constrainedPlan.valid = false;
        constrainedPlan.conflicts = [
            {
                code: 'SFS_RECORD_CAPACITY_EXHAUSTED',
                message: 'Partition 0 has 10 free SFS record slots but this plan requires 37; short by 27',
                packageIndex: null,
                rootIndex: null,
                packageId: '',
                nodeId: '',
                partitionIndex: 0,
                groupName: '',
                volumeName: '',
                rawGroup: '',
                rawVolume: '',
            },
        ];
        const capacity = constrainedPlan.sfsIndexCapacity[0];
        capacity.freeRecordSlots = 10;
        capacity.allocatedRecordSlots = 10;
        capacity.shortfallRecordSlots = 27;
        capacity.remainingRecordSlots = 0;
        capacity.packages[0].standaloneRequiredRecordSlots = 5010;
        capacity.packages[0].allocatedRecordSlots = 10;
        capacity.packages[0].shortfallRecordSlots = 11;
        capacity.packages[1].allocatedRecordSlots = 0;
        capacity.packages[1].shortfallRecordSlots = 16;

        render(PackageBatchImportDialog, { props: { ...props(), plan: constrainedPlan } });

        const summary = screen.getByLabelText('SFS record capacity for partition 1');
        expect(summary.textContent).toContain('10 free');
        expect(summary.textContent).toContain('37 required');
        expect(summary.textContent).toContain('27 short');
        expect(screen.getByText(/15 objects \+ 6 volume · 11 short/)).toBeTruthy();
        expect(screen.getByText(/16 objects · 16 short/)).toBeTruthy();
        expect(screen.queryByText(constrainedPlan.conflicts[0].message)).toBeNull();
        expect(screen.getByTitle('Requires 5010 records on an empty partition; maximum 5009')).toBeTruthy();
        expect(screen.getByRole('button', { name: 'Import 2 packages' }).hasAttribute('disabled')).toBe(true);
    });
});
