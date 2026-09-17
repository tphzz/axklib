import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import CompanionDiskDialog from './CompanionDiskDialog.svelte';

describe('CompanionDiskDialog', () => {
    it('shows the required disk for cataloged folders and keeps busy actions stable', async () => {
        const oncancel = vi.fn();
        const { rerender } = render(CompanionDiskDialog, {
            props: {
                sources: [],
                sourceKind: 'directory',
                setLabel: 'Set',
                nextRequiredIndex: 3,
                busy: false,
                error: '',
                onadd: vi.fn(),
                onremove: vi.fn(),
                onnearby: vi.fn(),
                onconfirm: vi.fn(),
                oncancel,
            },
        });
        expect(screen.getByText(/requires disk 3/)).toBeTruthy();
        expect(screen.queryByText(/Wave Data continues/)).toBeNull();
        const action = screen.getByRole('button', { name: 'Add and retry' });
        expect(action.parentElement?.classList.contains('dialog-footer-actions')).toBe(true);
        expect(screen.getByRole('status').classList.contains('dialog-footer-status')).toBe(true);
        await rerender({ busy: true });
        expect(screen.getByRole('button', { name: 'Add and retry' })).toBe(action);
        expect(screen.getByRole('status').textContent).toBe('Adding companions');
        expect((screen.getByRole('button', { name: 'Close' }) as HTMLButtonElement).disabled).toBe(true);
        await fireEvent.keyDown(document, { key: 'Escape' });
        expect(oncancel).not.toHaveBeenCalled();
    });
    it('collects explicit floppy images and keeps nearby discovery user initiated', async () => {
        const onadd = vi.fn();
        const onnearby = vi.fn();
        const onconfirm = vi.fn();
        const onremove = vi.fn();
        render(CompanionDiskDialog, {
            props: {
                sources: [
                    {
                        kind: 'server-file',
                        reference: { rootId: 'samples', relativePath: 'set/DISK1.ima' },
                        displayName: 'DISK1.ima',
                    },
                ],
                sourceKind: 'file',
                setLabel: 'YAMAHA SET',
                nextRequiredIndex: 2,
                busy: false,
                error: '',
                onadd,
                onremove,
                onnearby,
                onconfirm,
                oncancel: vi.fn(),
            },
        });

        expect(screen.getByText('set/DISK1.ima')).toBeTruthy();
        expect(
            screen.getByText(
                (_, element) =>
                    element?.tagName === 'P' && (element.textContent?.includes('requires disk 2.') ?? false),
            ),
        ).toBeTruthy();
        await fireEvent.click(screen.getByRole('button', { name: 'Add floppy image' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Search nearby images and retry' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Add and retry' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Remove set/DISK1.ima' }));

        expect(onadd).toHaveBeenCalledOnce();
        expect(onnearby).toHaveBeenCalledOnce();
        expect(onconfirm).toHaveBeenCalledOnce();
        expect(onremove).toHaveBeenCalledWith({
            kind: 'server-file',
            reference: { rootId: 'samples', relativePath: 'set/DISK1.ima' },
            displayName: 'DISK1.ima',
        });
    });

    it('requires an explicit folder before selected-folder retry', () => {
        render(CompanionDiskDialog, {
            props: {
                sources: [],
                sourceKind: 'directory',
                setLabel: 'Extracted disk recovery',
                nextRequiredIndex: null,
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
