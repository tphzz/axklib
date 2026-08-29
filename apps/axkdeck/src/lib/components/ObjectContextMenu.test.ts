import { fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import ObjectContextMenu from './ObjectContextMenu.svelte';

describe('ObjectContextMenu', () => {
    it('offers one Sample Bank assignment command for new or existing targets', async () => {
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

    it('orders mutations before deletion and groups every export in a submenu', async () => {
        const onexportwav = vi.fn();
        const onclose = vi.fn();
        render(ObjectContextMenu, {
            props: {
                objectName: 'Sample',
                left: 20,
                top: 30,
                onrename: vi.fn(),
                onassignsamplebank: vi.fn(),
                ondelete: vi.fn(),
                onexportpackage: vi.fn(),
                onexportwav,
                onexportsfz: vi.fn(),
                onexportmidi: vi.fn(),
                onclose,
            },
        });

        const root = screen.getByRole('menu', { name: 'Sample actions' });
        expect(
            within(root)
                .getAllByRole('menuitem')
                .map((item) => item.textContent?.trim()),
        ).toEqual(['Rename…', 'Assign to Sample Bank…', 'Delete…', 'Export']);
        expect(root.querySelectorAll(':scope > [role="separator"]')).toHaveLength(1);
        expect(screen.queryByRole('menuitem', { name: 'Export WAV…' })).toBeNull();

        await fireEvent.click(within(root).getByRole('menuitem', { name: 'Export' }));
        const exportMenu = screen.getByRole('menu', { name: 'Export actions' });
        expect(
            within(exportMenu)
                .getAllByRole('menuitem')
                .map((item) => item.textContent?.trim()),
        ).toEqual(['Export package…', 'Export WAV…', 'Export SFZ…', 'Export MIDI…']);
        const wav = within(exportMenu).getByRole('menuitem', { name: 'Export WAV…' });
        await fireEvent.click(wav);
        expect(onexportwav).toHaveBeenCalledOnce();
        expect(onclose).toHaveBeenCalledOnce();
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

        const rename = screen.getByRole('menuitem', { name: 'Rename…' });
        const deleteItem = screen.getByRole('menuitem', { name: 'Delete…' });
        const exportItem = screen.getByRole('menuitem', { name: 'Export' });
        await waitFor(() => expect(document.activeElement).toBe(rename));
        expect(rename.tabIndex).toBe(0);
        expect(deleteItem.tabIndex).toBe(-1);

        await fireEvent.keyDown(rename, { key: 'ArrowDown' });
        expect(document.activeElement).toBe(deleteItem);
        await fireEvent.keyDown(deleteItem, { key: 'ArrowDown' });
        expect(document.activeElement).toBe(exportItem);
        await fireEvent.keyDown(exportItem, { key: 'ArrowRight' });
        const exportPackage = screen.getByRole('menuitem', { name: 'Export package…' });
        expect(document.activeElement).toBe(exportPackage);
        await fireEvent.keyDown(exportPackage, { key: 'ArrowLeft' });
        await waitFor(() => expect(document.activeElement).toBe(exportItem));
        await fireEvent.keyDown(exportItem, { key: 'Home' });
        expect(document.activeElement).toBe(rename);
        await fireEvent.keyDown(rename, { key: 'ArrowUp' });
        expect(document.activeElement).toBe(exportItem);
        await fireEvent.keyDown(exportItem, { key: 'Escape' });
        expect(onclose).toHaveBeenCalledOnce();

        rendered.unmount();
        expect(document.activeElement).toBe(invoker);
        invoker.remove();
    });
});
