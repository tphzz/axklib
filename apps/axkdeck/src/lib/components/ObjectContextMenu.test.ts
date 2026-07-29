import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import ObjectContextMenu from './ObjectContextMenu.svelte';

describe('ObjectContextMenu', () => {
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
