import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import ObjectContextMenu from './ObjectContextMenu.svelte';

describe('ObjectContextMenu', () => {
    it('offers Sample Bank creation as a selection command', async () => {
        const oncreatesamplebank = vi.fn();
        render(ObjectContextMenu, {
            props: {
                objectName: '2 Samples',
                selectionCount: 2,
                left: 20,
                top: 30,
                oncreatesamplebank,
                onclose: vi.fn(),
            },
        });

        await fireEvent.click(screen.getByRole('menuitem', { name: 'Create Sample Bank from selection…' }));
        expect(oncreatesamplebank).toHaveBeenCalledOnce();
    });

    it('offers assignment to an existing Sample Bank as a selection command', async () => {
        const onassignsamplebank = vi.fn();
        render(ObjectContextMenu, {
            props: {
                objectName: '2 Samples',
                selectionCount: 2,
                left: 20,
                top: 30,
                onassignsamplebank,
                onclose: vi.fn(),
            },
        });

        await fireEvent.click(screen.getByRole('menuitem', { name: 'Assign to Sample Bank…' }));
        expect(onassignsamplebank).toHaveBeenCalledOnce();
    });

    it('offers direct WAV export immediately before SFZ export', async () => {
        const onexportwav = vi.fn();
        render(ObjectContextMenu, {
            props: {
                objectName: 'Sample',
                left: 20,
                top: 30,
                onexportwav,
                onexportsfz: vi.fn(),
                onclose: vi.fn(),
            },
        });

        const wav = screen.getByRole('menuitem', { name: 'Export WAV…' });
        const sfz = screen.getByRole('menuitem', { name: 'Export SFZ…' });
        expect(wav.compareDocumentPosition(sfz) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy();
        await fireEvent.click(wav);
        expect(onexportwav).toHaveBeenCalledOnce();
    });

    it('uses roving keyboard focus and restores the invoking control', async () => {
        const invoker = document.createElement('button');
        document.body.append(invoker);
        invoker.focus();
        const onclose = vi.fn();
        const rendered = render(ObjectContextMenu, {
            props: {
                objectName: 'Sample',
                left: 20,
                top: 30,
                onrename: vi.fn(),
                onexportpackage: vi.fn(),
                ondelete: vi.fn(),
                onclose,
            },
        });

        const rename = screen.getByRole('menuitem', { name: 'Rename' });
        const exportPackage = screen.getByRole('menuitem', { name: 'Export package…' });
        const deleteItem = screen.getByRole('menuitem', { name: 'Delete' });
        await waitFor(() => expect(document.activeElement).toBe(rename));
        expect(rename.tabIndex).toBe(0);
        expect(exportPackage.tabIndex).toBe(-1);

        await fireEvent.keyDown(rename, { key: 'ArrowDown' });
        expect(document.activeElement).toBe(exportPackage);
        await fireEvent.keyDown(exportPackage, { key: 'End' });
        expect(document.activeElement).toBe(deleteItem);
        await fireEvent.keyDown(deleteItem, { key: 'Home' });
        expect(document.activeElement).toBe(rename);
        await fireEvent.keyDown(rename, { key: 'ArrowUp' });
        expect(document.activeElement).toBe(deleteItem);
        await fireEvent.keyDown(deleteItem, { key: 'Escape' });
        expect(onclose).toHaveBeenCalledOnce();

        rendered.unmount();
        expect(document.activeElement).toBe(invoker);
        invoker.remove();
    });
});
