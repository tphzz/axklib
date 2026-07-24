/// <reference types="node" />

import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import type { ObjectDeletionInspection } from '../transport';
import ObjectDeletionDialog from './ObjectDeletionDialog.svelte';

const appStyles = readFileSync(resolve(process.cwd(), 'src/app.css'), 'utf8');

const inspection: ObjectDeletionInspection = {
    valid: true,
    imageId: 'image-1',
    revision: 2,
    targetObjectId: 'sample-1',
    selectedObjectIds: ['sample-1'],
    impacts: [
        {
            objectId: 'sample-1',
            objectType: 'SBNK',
            objectName: 'Piano C3',
            partitionIndex: 0,
            partitionName: 'Partition 0',
            volumeName: 'Piano',
            role: 'TARGET',
            status: 'REQUIRED',
            selected: true,
            storedSizeBytes: 512,
            freedClusters: 2,
            prerequisiteObjectIds: [],
            reason: 'Requested deletion target',
        },
        {
            objectId: 'wave-1',
            objectType: 'SMPL',
            objectName: 'Piano C3 L',
            partitionIndex: 0,
            partitionName: 'Partition 0',
            volumeName: 'Piano',
            role: 'DEPENDENCY',
            status: 'OPTIONAL',
            selected: false,
            storedSizeBytes: 4096,
            freedClusters: 4,
            prerequisiteObjectIds: ['sample-1'],
            reason: 'Referenced only by the selected Sample',
        },
    ],
    references: [
        {
            sourceObjectId: 'sample-1',
            sourceObjectType: 'SBNK',
            sourceObjectName: 'Piano C3',
            targetObjectId: 'wave-1',
            targetObjectType: 'SMPL',
            targetObjectName: 'Piano C3 L',
            type: 'SAMPLE_WAVE_DATA',
            quality: 'CONFIRMED',
            effect: 'REMOVED',
        },
    ],
    blockers: [],
    warnings: [
        {
            code: 'WAVE_DATA_WILL_BE_UNREFERENCED',
            message: 'Selected Sample deletion will leave valid unreferenced Wave Data',
            objectIds: ['wave-1'],
        },
    ],
    estimatedFreedBytes: 2048,
    estimatedFreedClusters: 2,
};

describe('ObjectDeletionDialog', () => {
    it('separates the requested deletion from conservative optional cleanup', async () => {
        const onselectionchange = vi.fn();
        const onselectall = vi.fn();
        const onconfirm = vi.fn();
        render(ObjectDeletionDialog, {
            props: {
                targetName: 'Piano C3',
                inspection,
                loading: false,
                busy: false,
                error: '',
                onselectionchange,
                onselectall,
                oncancel: vi.fn(),
                onconfirm,
            },
        });

        expect(screen.getByRole('dialog', { name: 'Delete Sample' })).toBeTruthy();
        expect(screen.getByRole('heading', { name: 'Will be deleted' })).toBeTruthy();
        expect(screen.getByRole('heading', { name: 'Optional cleanup' })).toBeTruthy();
        expect(
            screen.getByText('These related objects remain valid if kept. Select any you also want to delete.'),
        ).toBeTruthy();
        expect(screen.queryByRole('heading', { name: 'Affected objects' })).toBeNull();
        expect(screen.queryByText('Required')).toBeNull();
        expect(screen.queryByText(/SAMPLE_WAVE_DATA/)).toBeNull();
        expect(screen.queryByText('Selected Sample deletion will leave valid unreferenced Wave Data')).toBeNull();
        expect(screen.getByText(/1 object will be deleted/)).toBeTruthy();
        expect(screen.getByText(/2 KiB freed/)).toBeTruthy();

        await fireEvent.click(screen.getByRole('checkbox', { name: 'Also delete all (1)' }));
        expect(onselectall).toHaveBeenCalledWith(true);
        await fireEvent.click(screen.getByRole('checkbox', { name: 'Delete Wave Data Piano C3 L' }));
        expect(onselectionchange).toHaveBeenCalledWith('wave-1', true);
        await fireEvent.click(screen.getByRole('button', { name: 'Delete 1 object' }));
        expect(onconfirm).toHaveBeenCalledOnce();
    });

    it('surfaces blockers and their consequential references without technical labels', () => {
        render(ObjectDeletionDialog, {
            props: {
                targetName: 'Piano C3',
                inspection: {
                    ...inspection,
                    valid: false,
                    selectedObjectIds: [],
                    impacts: [{ ...inspection.impacts[0]!, status: 'BLOCKED' }],
                    blockers: [
                        {
                            code: 'incoming_reference',
                            message: 'Program 001 still refers to this Sample.',
                            objectIds: ['program-1', 'sample-1'],
                        },
                    ],
                    references: [
                        {
                            sourceObjectId: 'program-1',
                            sourceObjectType: 'PROG',
                            sourceObjectName: '001: Piano',
                            targetObjectId: 'sample-1',
                            targetObjectType: 'SBNK',
                            targetObjectName: 'Piano C3',
                            type: 'PROG_ASSIGNMENT_TO_SBNK',
                            quality: 'Known',
                            effect: 'BLOCKING',
                        },
                    ],
                },
                loading: false,
                busy: false,
                error: '',
                onselectionchange: vi.fn(),
                onselectall: vi.fn(),
                oncancel: vi.fn(),
                onconfirm: vi.fn(),
            },
        });

        expect(screen.getByRole('region', { name: 'Deletion blockers' })).toBeTruthy();
        expect(screen.getByRole('heading', { name: 'Requested deletion' })).toBeTruthy();
        expect(screen.getByText('Program 001 still refers to this Sample.')).toBeTruthy();
        expect(screen.getByText('Referenced by Program 001: Piano')).toBeTruthy();
        expect(screen.queryByRole('heading', { name: 'References' })).toBeNull();
        expect(screen.queryByText(/Known/)).toBeNull();
        expect(screen.queryByText(/PROG_ASSIGNMENT_TO_SBNK/)).toBeNull();
        expect(screen.getByRole('button', { name: 'Delete 1 object' }).hasAttribute('disabled')).toBe(true);
    });

    it('nests optional Wave Data and disables it until its prerequisite Sample is selected', () => {
        const optionalSample = {
            ...inspection.impacts[0]!,
            objectId: 'sample-2',
            objectName: 'Child Sample',
            role: 'DEPENDENCY' as const,
            status: 'OPTIONAL' as const,
            selected: false,
            prerequisiteObjectIds: [],
            reason: 'Remains as a standalone Sample if kept',
        };
        const optionalWave = {
            ...inspection.impacts[1]!,
            prerequisiteObjectIds: ['sample-2'],
        };
        render(ObjectDeletionDialog, {
            props: {
                targetName: 'Bank',
                inspection: {
                    ...inspection,
                    targetObjectId: 'bank-1',
                    selectedObjectIds: ['bank-1'],
                    impacts: [
                        { ...inspection.impacts[0]!, objectId: 'bank-1', objectType: 'SBAC' },
                        optionalSample,
                        optionalWave,
                    ],
                },
                loading: false,
                busy: false,
                error: '',
                onselectionchange: vi.fn(),
                onselectall: vi.fn(),
                oncancel: vi.fn(),
                onconfirm: vi.fn(),
            },
        });

        const waveCheckbox = screen.getByRole('checkbox', { name: 'Delete Wave Data Piano C3 L' });
        expect(waveCheckbox.hasAttribute('disabled')).toBe(true);
        expect(screen.getByText('Available after deleting Child Sample')).toBeTruthy();
        expect(waveCheckbox.closest('.deletion-impact')?.classList.contains('nested')).toBe(true);
    });

    it('shows preserved related objects and folds external references into their rows', () => {
        render(ObjectDeletionDialog, {
            props: {
                targetName: 'Bank',
                inspection: {
                    ...inspection,
                    targetObjectId: 'bank-1',
                    selectedObjectIds: ['bank-1'],
                    impacts: [
                        { ...inspection.impacts[0]!, objectId: 'bank-1', objectType: 'SBAC', objectName: 'Bank' },
                        {
                            ...inspection.impacts[0]!,
                            objectId: 'sample-2',
                            objectName: 'Program Sample',
                            role: 'DEPENDENCY',
                            status: 'PRESERVED',
                            selected: false,
                            reason: 'Sample remains because a Program references it',
                        },
                    ],
                    references: [
                        {
                            sourceObjectId: 'program-1',
                            sourceObjectType: 'PROG',
                            sourceObjectName: '001: Piano',
                            targetObjectId: 'sample-2',
                            targetObjectType: 'SBNK',
                            targetObjectName: 'Program Sample',
                            type: 'PROG_ASSIGNMENT_TO_SBNK',
                            quality: 'Known',
                            effect: 'PRESERVED',
                        },
                    ],
                },
                loading: false,
                busy: false,
                error: '',
                onselectionchange: vi.fn(),
                onselectall: vi.fn(),
                oncancel: vi.fn(),
                onconfirm: vi.fn(),
            },
        });

        expect(screen.getByRole('heading', { name: 'Will remain' })).toBeTruthy();
        expect(screen.getByText('Program Sample')).toBeTruthy();
        expect(screen.getByText('Sample remains because a Program references it')).toBeTruthy();
        expect(screen.getByText('Referenced by Program 001: Piano')).toBeTruthy();
        expect(screen.queryByRole('heading', { name: 'References' })).toBeNull();
        expect(screen.queryByText(/Known/)).toBeNull();
        expect(screen.queryByText(/PROG_ASSIGNMENT_TO_SBNK/)).toBeNull();
    });

    it('reports the selected cleanup count without changing the dialog structure', () => {
        render(ObjectDeletionDialog, {
            props: {
                targetName: 'Piano C3',
                inspection: {
                    ...inspection,
                    selectedObjectIds: ['sample-1', 'wave-1'],
                    impacts: inspection.impacts.map((impact) =>
                        impact.objectId === 'wave-1' ? { ...impact, selected: true } : impact,
                    ),
                    warnings: [],
                    estimatedFreedBytes: 496 * 1024,
                    estimatedFreedClusters: 496,
                },
                loading: false,
                busy: false,
                error: '',
                onselectionchange: vi.fn(),
                onselectall: vi.fn(),
                oncancel: vi.fn(),
                onconfirm: vi.fn(),
            },
        });

        expect(screen.getByRole('heading', { name: 'Will be deleted' })).toBeTruthy();
        expect(screen.getByRole('heading', { name: 'Optional cleanup' })).toBeTruthy();
        expect(screen.getByText(/2 objects will be deleted/)).toBeTruthy();
        expect(screen.getByText(/496 KiB freed/)).toBeTruthy();
        expect(screen.getByRole('button', { name: 'Delete 2 objects' })).toBeTruthy();
    });

    it('shows a mixed optional-cleanup selection on the master checkbox', async () => {
        render(ObjectDeletionDialog, {
            props: {
                targetName: 'Piano C3',
                inspection: {
                    ...inspection,
                    selectedObjectIds: ['sample-1', 'wave-1'],
                    impacts: [
                        inspection.impacts[0]!,
                        { ...inspection.impacts[1]!, selected: true },
                        {
                            ...inspection.impacts[1]!,
                            objectId: 'wave-2',
                            objectName: 'Piano C3 R',
                        },
                    ],
                },
                loading: false,
                busy: false,
                error: '',
                onselectionchange: vi.fn(),
                onselectall: vi.fn(),
                oncancel: vi.fn(),
                onconfirm: vi.fn(),
            },
        });

        const master = screen.getByRole('checkbox', { name: 'Also delete all (2)' }) as HTMLInputElement;
        await vi.waitFor(() => expect(master.indeterminate).toBe(true));
        expect(master.checked).toBe(false);
    });

    it('uses whitespace for section boundaries and only divides adjacent object rows', () => {
        const listRule = appStyles.match(/\.deletion-impact-list\s*\{([^}]+)\}/)?.[1];
        const impactRule = appStyles.match(/\.deletion-impact\s*\{([^}]+)\}/)?.[1];
        const siblingRule = appStyles.match(/\.deletion-impact \+ \.deletion-impact\s*\{([^}]+)\}/)?.[1];
        const selectAllRule = appStyles.match(/\.deletion-select-all\s*\{([^}]+)\}/)?.[1];

        expect(listRule).toBeDefined();
        expect(listRule).not.toMatch(/border-(?:top|bottom)/);
        expect(impactRule).toBeDefined();
        expect(impactRule).not.toMatch(/border-(?:top|bottom)/);
        expect(siblingRule).toMatch(/border-top:\s*1px solid var\(--color-border\)/);
        expect(selectAllRule).toBeDefined();
        expect(selectAllRule).not.toMatch(/border-(?:top|bottom)/);
        expect(selectAllRule).toMatch(/background:/);
    });
});
