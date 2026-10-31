import { fireEvent, render, screen } from '@testing-library/svelte';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import type { components } from '../generated/axklibApiV1';
import AllocationGrid from './AllocationGrid.svelte';

const map = {
    clusterCount: 4096,
    partitionStartSector: 0,
    clusterSizeBytes: 4096,
    sectorSizeBytes: 512,
    sectorsPerCluster: 8,
    runs: [
        {
            startCluster: 0,
            clusterCount: 4096,
            allocationKind: 'CONFLICT',
            consistencyFlags: ['MULTIPLE_CLAIMS'],
            owners: Array.from({ length: 15 }, (_, i) => ({
                objectName: `Owner ${i}`,
                objectType: 'SMPL',
                recordKind: 'OBJECT',
                volumeName: 'Volume',
                sfsId: i + 1,
                claimKind: 'DATA',
                extentIndex: 0,
            })),
        },
    ],
} as components['schemas']['ImageAllocationMap'];

describe('allocation cluster inspection', () => {
    beforeEach(() => vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue(null));
    it('does not reveal the active cluster again during ordinary viewport scrolling', async () => {
        const { container } = render(AllocationGrid, { map, cellSize: 7 });
        screen.getByRole('slider', { name: 'Partition cluster' }).focus();
        const viewport = container.querySelector('.allocation-viewport')!;
        viewport.scrollTop = 100;
        await fireEvent.scroll(viewport);
        expect(viewport.scrollTop).toBe(100);
    });
    it('navigates cluster boundaries, pins complete details and restores map focus on Escape', async () => {
        const { unmount } = render(AllocationGrid, { map, cellSize: 7 });
        const canvas = screen.getByRole('slider', { name: 'Partition cluster' });
        canvas.focus();
        await fireEvent.focus(canvas);
        expect(canvas.getAttribute('aria-valuenow')).toBe('0');
        await fireEvent.keyDown(canvas, { key: 'ArrowRight' });
        expect(canvas.getAttribute('aria-valuenow')).toBe('1');
        await fireEvent.keyDown(canvas, { key: 'End' });
        expect(canvas.getAttribute('aria-valuenow')).toBe('4095');
        await fireEvent.keyDown(canvas, { key: 'ArrowRight' });
        expect(canvas.getAttribute('aria-valuenow')).toBe('4095');
        await fireEvent.keyDown(canvas, { key: 'Home' });
        await fireEvent.keyDown(canvas, { key: 'Enter' });
        const details = await screen.findByRole('region', { name: 'Partition cluster details' });
        expect(details.getAttribute('tabindex')).toBe('0');
        expect(details.textContent).toContain('Owner 14');
        await fireEvent.mouseMove(canvas, { clientX: 80, clientY: 80 });
        expect(canvas.getAttribute('aria-valuenow')).toBe('0');
        await fireEvent.keyDown(details, { key: 'Escape' });
        expect(screen.queryByRole('region', { name: 'Partition cluster details' })).toBeNull();
        expect(document.activeElement).toBe(canvas);
        unmount();
        expect(screen.queryByRole('tooltip')).toBeNull();
    });

    it('dismisses pinned details on focus departure and does not inspect empty maps', async () => {
        const { rerender } = render(AllocationGrid, { map, cellSize: 7 });
        const canvas = screen.getByRole('slider', { name: 'Partition cluster' });
        await fireEvent.focus(canvas);
        await fireEvent.keyDown(canvas, { key: 'Enter' });
        await screen.findByRole('region', { name: 'Partition cluster details' });
        const other = document.createElement('button');
        document.body.append(other);
        await fireEvent.focusIn(other);
        expect(screen.queryByRole('region', { name: 'Partition cluster details' })).toBeNull();
        other.remove();
        await rerender({ map: { ...map, clusterCount: 0, runs: [] } });
        expect(canvas.getAttribute('aria-disabled')).toBe('true');
        await fireEvent.keyDown(canvas, { key: 'Enter' });
        expect(screen.queryByRole('region', { name: 'Partition cluster details' })).toBeNull();
    });
});
