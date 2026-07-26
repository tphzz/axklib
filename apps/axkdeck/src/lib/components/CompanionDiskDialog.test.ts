import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import CompanionDiskDialog from './CompanionDiskDialog.svelte';

describe('CompanionDiskDialog', () => {
    it('collects explicit folders and keeps nearby discovery user initiated', async () => {
        const onadd = vi.fn();
        const onnearby = vi.fn();
        const onconfirm = vi.fn();
        const onremove = vi.fn();
        render(CompanionDiskDialog, {
            props: {
                directories: [{ rootId: 'samples', relativePath: 'set/DISK1' }],
                busy: false,
                error: '',
                onadd,
                onremove,
                onnearby,
                onconfirm,
                oncancel: vi.fn(),
            },
        });

        expect(screen.getByText('set/DISK1')).toBeTruthy();
        await fireEvent.click(screen.getByRole('button', { name: 'Add folder' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Search nearby folders and retry' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Add and retry' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Remove set/DISK1' }));

        expect(onadd).toHaveBeenCalledOnce();
        expect(onnearby).toHaveBeenCalledOnce();
        expect(onconfirm).toHaveBeenCalledOnce();
        expect(onremove).toHaveBeenCalledWith({ rootId: 'samples', relativePath: 'set/DISK1' });
    });

    it('requires an explicit folder before selected-folder retry', () => {
        render(CompanionDiskDialog, {
            props: {
                directories: [],
                busy: false,
                error: '',
                onadd: vi.fn(),
                onremove: vi.fn(),
                onnearby: vi.fn(),
                onconfirm: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        expect((screen.getByRole('button', { name: 'Add and retry' }) as HTMLButtonElement).disabled).toBe(true);
        expect(
            (screen.getByRole('button', { name: 'Search nearby folders and retry' }) as HTMLButtonElement).disabled,
        ).toBe(false);
    });
});
