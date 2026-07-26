import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import type { WaveDataOrphanInspection } from '../transport';
import WaveDataCleanupDialog from './WaveDataCleanupDialog.svelte';

const inspection: WaveDataOrphanInspection = {
    imageId: 'image-1',
    revision: 3,
    contentScopeId: 'volume-1',
    totalCandidateCount: 2,
    candidates: [
        {
            objectId: 'wave-10',
            objectType: 'SMPL',
            objectName: 'Unused 10',
            partitionIndex: 0,
            partitionName: 'Partition 0',
            volumeName: 'Piano',
            storedSizeBytes: 1024,
            recoverableBytes: 4096,
            recoverableClusters: 1,
        },
        {
            objectId: 'wave-2',
            objectType: 'SMPL',
            objectName: 'Unused 2',
            partitionIndex: 0,
            partitionName: 'Partition 0',
            volumeName: 'Piano',
            storedSizeBytes: 2048,
            recoverableBytes: 8192,
            recoverableClusters: 2,
        },
    ],
};

describe('WaveDataCleanupDialog', () => {
    it('reviews naturally ordered safe candidates and defaults to deleting all', async () => {
        const onselectionchange = vi.fn();
        const onselectall = vi.fn();
        const onconfirm = vi.fn();
        render(WaveDataCleanupDialog, {
            props: {
                volumeName: 'Piano',
                inspection,
                selectedObjectIds: ['wave-10', 'wave-2'],
                loading: false,
                busy: false,
                error: '',
                onselectionchange,
                onselectall,
                oncancel: vi.fn(),
                onconfirm,
            },
        });

        expect(screen.getByRole('dialog', { name: 'Clean up Wave Data' })).toBeTruthy();
        expect(screen.getByText('Only Wave Data that can be verified as unused is shown.')).toBeTruthy();
        const candidateNames = [...document.querySelectorAll('.wave-data-cleanup-name')].map(
            (element) => element.textContent,
        );
        expect(candidateNames).toEqual(['Unused 2', 'Unused 10']);
        expect((screen.getByRole('checkbox', { name: 'Delete all (2)' }) as HTMLInputElement).checked).toBe(true);
        expect(screen.getByText(/12 KiB can be recovered/)).toBeTruthy();

        await fireEvent.click(screen.getByRole('checkbox', { name: 'Delete Wave Data Unused 2' }));
        expect(onselectionchange).toHaveBeenCalledWith('wave-2', false);
        await fireEvent.click(screen.getByRole('checkbox', { name: 'Delete all (2)' }));
        expect(onselectall).toHaveBeenCalledWith(false);
        await fireEvent.click(screen.getByRole('button', { name: 'Delete 2 Wave Data objects' }));
        expect(onconfirm).toHaveBeenCalledOnce();
    });

    it('uses a calm empty state and explains when the response is capped', () => {
        const rendered = render(WaveDataCleanupDialog, {
            props: {
                volumeName: 'Piano',
                inspection: { ...inspection, candidates: [], totalCandidateCount: 0 },
                selectedObjectIds: [],
                loading: false,
                busy: false,
                error: '',
                onselectionchange: vi.fn(),
                onselectall: vi.fn(),
                oncancel: vi.fn(),
                onconfirm: vi.fn(),
            },
        });
        expect(screen.getByText('No unreferenced Wave Data found in Piano.')).toBeTruthy();
        expect(screen.getAllByRole('button', { name: 'Close' })).toHaveLength(2);

        rendered.rerender({
            volumeName: 'Piano',
            inspection: { ...inspection, totalCandidateCount: 1200 },
            selectedObjectIds: ['wave-10', 'wave-2'],
            loading: false,
            busy: false,
            error: '',
            onselectionchange: vi.fn(),
            onselectall: vi.fn(),
            oncancel: vi.fn(),
            onconfirm: vi.fn(),
        });
        expect(
            screen.getByText('Showing the first 2 of 1,200 candidates. Clean up again for the remainder.'),
        ).toBeTruthy();
    });
});
