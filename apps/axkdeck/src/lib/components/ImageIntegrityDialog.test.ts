import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import ImageIntegrityDialog from './ImageIntegrityDialog.svelte';

describe('ImageIntegrityDialog', () => {
    it('explains guarded EX access without offering repair or disabling all writes', async () => {
        const onclose = vi.fn();
        render(ImageIntegrityDialog, {
            props: {
                issues: [
                    {
                        code: 'EX5_CAPACITY_EXCEEDS_IMAGE',
                        severity: 'WARNING',
                        message: 'EX5 filesystem declares one sector beyond the image.',
                        objectId: null,
                        samplerPath: '/',
                    },
                ],
                loading: false,
                error: '',
                repairAvailable: false,
                repairing: false,
                repairLabel: '',
                repairError: '',
                onrepair: vi.fn(),
                onclose,
            },
        });
        expect(screen.getByText('EX image size mismatch')).toBeTruthy();
        expect(screen.getByText(/Guarded writing remains available/)).toBeTruthy();
        expect(screen.getByText(/EX hardware may still attempt/)).toBeTruthy();
        expect(screen.queryByText(/No blocking SFS/)).toBeNull();
        expect(screen.queryByText(/Alteration is disabled/)).toBeNull();
        expect(screen.queryByRole('button', { name: /repair/i })).toBeNull();
        await fireEvent.click(screen.getByRole('button', { name: 'Close' }));
        expect(onclose).toHaveBeenCalledOnce();
        await fireEvent.keyDown(screen.getByRole('dialog'), { key: 'Escape' });
        expect(onclose).toHaveBeenCalledTimes(2);
    });
    it('explains allocation blockers without offering a repair action', async () => {
        const onclose = vi.fn();
        render(ImageIntegrityDialog, {
            props: {
                issues: [
                    {
                        code: 'SFS_ALLOCATION_BITMAP_COPIES_DIFFER',
                        message: 'Stored allocation bitmap copies differ',
                        objectId: null,
                        samplerPath: 'Partition 0',
                        severity: 'ERROR',
                    },
                ],
                loading: false,
                error: '',
                repairAvailable: false,
                repairing: false,
                repairLabel: '',
                repairError: '',
                onrepair: vi.fn(),
                onclose,
            },
        });

        expect(screen.getByText('Alteration is disabled for this image')).toBeTruthy();
        expect(screen.getByText(/Browsing and export remain available/)).toBeTruthy();
        expect(screen.queryByRole('button', { name: /repair/i })).toBeNull();
        await fireEvent.click(screen.getByRole('button', { name: 'Close' }));
        expect(onclose).toHaveBeenCalledOnce();
    });

    it('offers copy-only repair for supported extent byte-total defects', async () => {
        const onrepair = vi.fn();
        render(ImageIntegrityDialog, {
            props: {
                issues: [
                    {
                        code: 'SFS_EXTENT_BYTE_TOTAL_MISMATCH',
                        message: 'Extent byte total does not match the logical record size',
                        objectId: null,
                        samplerPath: 'Partition 1 / small2-ren',
                        severity: 'ERROR',
                    },
                    {
                        code: 'SFS_EXTENT_BYTE_TOTAL_MISMATCH',
                        message: 'Extent byte total does not match the logical record size',
                        objectId: null,
                        samplerPath: 'Partition 1 / Wave Data',
                        severity: 'ERROR',
                    },
                ],
                loading: false,
                error: '',
                repairAvailable: true,
                repairing: false,
                repairLabel: '',
                repairError: '',
                onrepair,
                onclose: vi.fn(),
            },
        });

        expect(screen.getByText(/source image remains unchanged/i)).toBeTruthy();
        await fireEvent.click(screen.getByRole('button', { name: 'Repair copy...' }));
        expect(onrepair).toHaveBeenCalledOnce();
    });

    it('reports a clean image without an allocation warning', () => {
        render(ImageIntegrityDialog, {
            props: {
                issues: [],
                loading: false,
                error: '',
                repairAvailable: false,
                repairing: false,
                repairLabel: '',
                repairError: '',
                onrepair: vi.fn(),
                onclose: vi.fn(),
            },
        });

        expect(screen.getByText('No blocking SFS allocation issue was found.')).toBeTruthy();
        expect(screen.getByText('The image has no reported validation issues.')).toBeTruthy();
    });
});
