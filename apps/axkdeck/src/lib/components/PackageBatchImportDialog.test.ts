import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import { serverFileLocation } from '../storageLocations';
import type { ImageSessionPackageImportPlan, PackageInspection } from '../transport';
import type { BatchPackageItem } from '../../features/import/packageBatchWorkflow.svelte';
import PackageBatchImportDialog from './PackageBatchImportDialog.svelte';

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
};

const callbacks = {
    onchooseworkspace: vi.fn(),
    onchooselocal: vi.fn(),
    onrename: vi.fn(),
    onremove: vi.fn(),
    onopaquesequenceaction: vi.fn(),
    onreplan: vi.fn(),
    oncancel: vi.fn(),
    onconfirm: vi.fn(),
};

function props() {
    return {
        partitionName: 'Partition 1',
        desktop: false,
        items,
        plan,
        volumeNames: { 0: 'Drums', 1: 'Drums 2' },
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
    it('previews every destination and its object counts before one batch import', async () => {
        render(PackageBatchImportDialog, { props: props() });

        expect(screen.getByRole('heading', { name: 'Import volume packages' })).toBeTruthy();
        expect(screen.getByText('2 volumes will be created')).toBeTruthy();
        expect(screen.getAllByText('1 Programs')).toHaveLength(2);
        expect(screen.getAllByText('2 Sample Banks')).toHaveLength(2);
        expect(screen.getAllByText('3 Samples')).toHaveLength(2);
        expect(screen.getAllByText('8 Wave Data')).toHaveLength(2);
        expect(screen.getAllByText('1 Sequences')).toHaveLength(2);
        expect(screen.getByRole('button', { name: 'Import 2 packages' }).hasAttribute('disabled')).toBe(false);

        await fireEvent.input(screen.getByLabelText('New volume name for two.axkvol'), {
            target: { value: 'Percussion' },
        });
        expect(callbacks.onrename).toHaveBeenCalledWith(1, 'Percussion');
    });

    it('requires edited destinations to be checked before import', () => {
        render(PackageBatchImportDialog, {
            props: { ...props(), volumeNames: { 0: 'Drums', 1: 'Percussion' }, hasUnvalidatedChanges: true },
        });

        expect(screen.getByRole('button', { name: 'Check conflicts' })).toBeTruthy();
        expect(screen.getByRole('button', { name: 'Import 2 packages' }).hasAttribute('disabled')).toBe(true);
    });

    it('does not report readiness while an apply error is visible', () => {
        render(PackageBatchImportDialog, {
            props: { ...props(), error: 'Alteration journal storage is full' },
        });

        expect(screen.queryByText('Ready to import')).toBeNull();
        expect(screen.getByRole('alert').textContent).toContain('Alteration journal storage is full');
    });
});
