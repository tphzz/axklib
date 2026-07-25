import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import type { ImageSessionPackageImportPlan, PackageInspection } from '../transport';
import PackageImportDialog from './PackageImportDialog.svelte';

const inspection: PackageInspection = {
    schemaVersion: '1.0',
    packageId: 'package-1',
    packageKind: 'VOLUME',
    requiredExtension: '.axkvol',
    sourceMediaKind: 'SFS',
    valid: true,
    payloadsVerified: true,
    totalPayloadBytes: 30 * 1024 * 1024,
    roots: [{ kind: 'VOLUME', displayName: 'DRUMS', nodeIds: ['bank-1'] }],
    objects: [
        {
            nodeId: 'bank-1',
            objectType: 'SBAC',
            name: 'Drum Bank',
            payloadSizeBytes: 4096,
            payloadSha256: 'a',
            normalizedSha256: 'b',
            semanticSha256: null,
            audioSha256: null,
        },
        {
            nodeId: 'sample-1',
            objectType: 'SBNK',
            name: 'Kick',
            payloadSizeBytes: 512,
            payloadSha256: 'c',
            normalizedSha256: 'd',
            semanticSha256: null,
            audioSha256: null,
        },
    ],
    relationships: [
        {
            edgeId: 'edge-1',
            sourceNodeId: 'bank-1',
            targetNodeId: 'sample-1',
            role: 'MEMBER',
            ordinal: 0,
        },
    ],
    relationshipCount: 1,
    issues: [],
};

function plan(valid = true): ImageSessionPackageImportPlan {
    return {
        schemaVersion: '1.0',
        imageId: 'image-1',
        revision: 2,
        planToken: 'plan-1',
        expiresInSeconds: 600,
        planId: 'plan-id',
        targetKind: 'SFS',
        targetSnapshotId: 'snapshot',
        valid,
        warnings: [],
        conflicts: valid
            ? []
            : [
                  {
                      code: 'NAME_CONFLICT',
                      message: 'Sample name already exists',
                      nodeId: 'sample-1',
                      packageId: 'package-1',
                      packageIndex: 0,
                      rootIndex: 0,
                      partitionIndex: 0,
                      groupName: '',
                      volumeName: 'TARGET',
                      rawGroup: '',
                      rawVolume: '',
                  },
              ],
        actions: [
            {
                actionId: 'action-1',
                packageIndex: 0,
                rootIndex: 0,
                packageId: 'package-1',
                nodeId: 'sample-1',
                objectType: 'SBNK',
                sourceName: 'Kick',
                destinationName: 'Kick',
                partitionIndex: 0,
                groupName: '',
                volumeName: 'TARGET',
                rawGroup: '',
                rawVolume: '',
                actions: valid ? ['INSERT'] : ['CONFLICT'],
                canonicalActionId: null,
                targetSfsId: null,
                targetWaveDataReferenceValue: null,
            },
        ],
        allocation: [
            {
                partitionIndex: 0,
                groupName: '',
                volumeName: 'TARGET',
                rawGroup: '',
                rawVolume: '',
                insertedObjectCount: valid ? 1 : 0,
                reusedObjectCount: 0,
                blockedObjectCount: valid ? 0 : 1,
                payloadClusters: valid ? 2 : 0,
                payloadSectors: 0,
                continuationClusters: 0,
                directoryGrowthBytes: valid ? 32 : 0,
                directoryGrowthClusters: 0,
                directoryContinuationClusters: 0,
                infrastructureClusters: 0,
                additionalAllocatedBytes: valid ? 2048 : 0,
                remainingObjectIds: 10,
                remainingClusters: 100,
                projectedImageSectors: 0,
                projectedImageSizeBytes: 0,
            },
        ],
    };
}

const callbacks = {
    onchooseworkspace: vi.fn(),
    onchooselocal: vi.fn(),
    onchange: vi.fn(),
    onrename: vi.fn(),
    onreplan: vi.fn(),
    oncancel: vi.fn(),
    onconfirm: vi.fn(),
};

describe('PackageImportDialog', () => {
    it('presents local and workspace sources before a package is selected', async () => {
        render(PackageImportDialog, {
            props: {
                targetName: 'TARGET',
                desktop: true,
                sourceName: '',
                inspection: null,
                plan: null,
                renames: {},
                status: 'choosing',
                progress: 0,
                error: '',
                ...callbacks,
            },
        });

        await fireEvent.click(screen.getByRole('button', { name: /Storage location/ }));
        await fireEvent.click(screen.getByRole('button', { name: /This computer/ }));
        expect(callbacks.onchooseworkspace).toHaveBeenCalledOnce();
        expect(callbacks.onchooselocal).toHaveBeenCalledOnce();
    });

    it('reviews the dependency tree and requires conflicts to be replanned', async () => {
        const onrename = vi.fn();
        const onreplan = vi.fn();
        render(PackageImportDialog, {
            props: {
                targetName: 'TARGET',
                desktop: true,
                sourceName: 'drums.axkvol',
                inspection,
                plan: plan(false),
                renames: {},
                status: 'ready',
                progress: 1,
                error: '',
                ...callbacks,
                onrename,
                onreplan,
            },
        });

        expect(screen.getByText('DRUMS')).toBeTruthy();
        expect(screen.getByText('Drum Bank')).toBeTruthy();
        expect(screen.getByText('Kick')).toBeTruthy();
        expect(screen.getByRole('heading', { name: 'Import into TARGET' })).toBeTruthy();
        expect((screen.getByRole('button', { name: 'Import package' }) as HTMLButtonElement).disabled).toBe(true);
        await fireEvent.input(screen.getByDisplayValue('Kick'), { target: { value: 'Kick 2' } });
        expect(onrename).toHaveBeenCalledWith('sample-1', 'Kick 2');
        await fireEvent.click(screen.getByRole('button', { name: 'Check names' }));
        expect(onreplan).toHaveBeenCalledOnce();
    });

    it('separates package payload size from allocated image space', () => {
        render(PackageImportDialog, {
            props: {
                targetName: 'TARGET',
                desktop: true,
                sourceName: 'drums.axkvol',
                inspection,
                plan: plan(),
                renames: {},
                status: 'ready',
                progress: 1,
                error: '',
                ...callbacks,
            },
        });

        expect(screen.getByText(/30\.0 MiB/)).toBeTruthy();
        expect(screen.getByText('Image space')).toBeTruthy();
        expect(screen.getByText('2 KiB')).toBeTruthy();
    });

    it('renders a shared dependency once in every owning branch without duplicate keys', () => {
        const sharedInspection: PackageInspection = {
            ...inspection,
            roots: [{ kind: 'VOLUME', displayName: 'DRUMS', nodeIds: ['bank-1', 'bank-2'] }],
            objects: [
                ...inspection.objects,
                {
                    nodeId: 'bank-2',
                    objectType: 'SBAC',
                    name: 'Second Drum Bank',
                    payloadSizeBytes: 4096,
                    payloadSha256: 'e',
                    normalizedSha256: 'f',
                    semanticSha256: null,
                    audioSha256: null,
                },
            ],
            relationships: [
                ...inspection.relationships,
                {
                    edgeId: 'edge-2',
                    sourceNodeId: 'bank-2',
                    targetNodeId: 'sample-1',
                    role: 'MEMBER',
                    ordinal: 0,
                },
            ],
            relationshipCount: 2,
        };

        render(PackageImportDialog, {
            props: {
                targetName: 'TARGET',
                desktop: true,
                sourceName: 'drums.axkvol',
                inspection: sharedInspection,
                plan: null,
                renames: {},
                status: 'planning',
                progress: 0,
                error: '',
                ...callbacks,
            },
        });

        expect(screen.getAllByText('Kick')).toHaveLength(2);
    });

    it('shows non-renamable plan blockers without offering a false naming remedy', () => {
        const blocked = plan(false);
        blocked.conflicts = [
            {
                ...blocked.conflicts[0],
                code: 'SFS_CAPACITY_EXCEEDED',
                nodeId: '',
                message: 'The destination does not have enough free clusters',
            },
        ];
        render(PackageImportDialog, {
            props: {
                targetName: 'TARGET',
                desktop: false,
                sourceName: 'drums.axkvol',
                inspection,
                plan: blocked,
                renames: {},
                status: 'ready',
                progress: 0,
                error: '',
                ...callbacks,
            },
        });

        expect(screen.getByText('1 issue prevents import')).toBeTruthy();
        expect(screen.getByText('The destination does not have enough free clusters')).toBeTruthy();
        expect(screen.queryByRole('button', { name: 'Check names' })).toBeNull();
    });

    it('groups repeated plan conflicts and node-scoped rename actions without duplicate keys', () => {
        const blocked = plan(false);
        blocked.actions.push({
            ...blocked.actions[0],
            actionId: 'action-2',
            rootIndex: 1,
        });
        const capacityConflict = {
            ...blocked.conflicts[0],
            code: 'SFS_DIRECTORY_CAPACITY_EXHAUSTED',
            nodeId: '',
            message: 'The destination directory does not have enough capacity',
        };
        blocked.conflicts.push(capacityConflict, { ...capacityConflict });

        render(PackageImportDialog, {
            props: {
                targetName: 'TARGET',
                desktop: false,
                sourceName: 'drums.axkvol',
                inspection,
                plan: blocked,
                renames: {},
                status: 'ready',
                progress: 0,
                error: '',
                ...callbacks,
            },
        });

        expect(screen.getAllByDisplayValue('Kick')).toHaveLength(1);
        expect(screen.getAllByText('The destination directory does not have enough capacity')).toHaveLength(1);
        expect(screen.getByText('2 issues prevent import')).toBeTruthy();
    });

    it.each(['loading', 'planning'] as const)('remains cancellable while package work is %s', async (status) => {
        const oncancel = vi.fn();
        render(PackageImportDialog, {
            props: {
                targetName: 'TARGET',
                desktop: true,
                sourceName: 'drums.axkvol',
                inspection: status === 'planning' ? inspection : null,
                plan: null,
                renames: {},
                status,
                progress: status === 'loading' ? 1 : 0,
                error: '',
                ...callbacks,
                oncancel,
            },
        });

        expect((screen.getByRole('button', { name: 'Close' }) as HTMLButtonElement).disabled).toBe(false);
        expect((screen.getByRole('button', { name: 'Change' }) as HTMLButtonElement).disabled).toBe(false);
        const cancel = screen.getByRole('button', { name: 'Cancel' }) as HTMLButtonElement;
        expect(cancel.disabled).toBe(false);
        await fireEvent.click(cancel);
        expect(oncancel).toHaveBeenCalledOnce();
    });

    it('locks cancellation only while the image mutation is applying', () => {
        render(PackageImportDialog, {
            props: {
                targetName: 'TARGET',
                desktop: true,
                sourceName: 'drums.axkvol',
                inspection,
                plan: plan(),
                renames: {},
                status: 'applying',
                progress: 1,
                error: '',
                ...callbacks,
            },
        });

        expect((screen.getByRole('button', { name: 'Close' }) as HTMLButtonElement).disabled).toBe(true);
        expect((screen.getByRole('button', { name: 'Change' }) as HTMLButtonElement).disabled).toBe(true);
        expect((screen.getByRole('button', { name: 'Cancel' }) as HTMLButtonElement).disabled).toBe(true);
    });
});
