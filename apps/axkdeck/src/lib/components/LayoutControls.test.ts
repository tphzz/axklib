import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import LayoutControls from './LayoutControls.svelte';

describe('LayoutControls', () => {
    it('exposes stable pressed toggles for both side panels', async () => {
        const ontogglelibrary = vi.fn();
        const ontoggleeditor = vi.fn();
        const ontoggleinspector = vi.fn();
        render(LayoutControls, {
            props: {
                libraryOpen: true,
                editorOpen: true,
                editorAvailable: true,
                inspectorOpen: false,
                ontogglelibrary,
                ontoggleeditor,
                ontoggleinspector,
            },
        });

        const library = screen.getByRole('button', { name: 'Library panel' });
        const inspector = screen.getByRole('button', { name: 'Inspector panel' });
        const editor = screen.getByRole('button', { name: 'Editor panel' });
        expect(library.getAttribute('aria-pressed')).toBe('true');
        expect(inspector.getAttribute('aria-pressed')).toBe('false');
        expect(editor.getAttribute('aria-pressed')).toBe('true');

        await fireEvent.click(library);
        await fireEvent.click(inspector);
        await fireEvent.click(editor);
        expect(ontogglelibrary).toHaveBeenCalledOnce();
        expect(ontoggleinspector).toHaveBeenCalledOnce();
        expect(ontoggleeditor).toHaveBeenCalledOnce();
    });

    it('disables the editor panel toggle for the Wave Data collection', () => {
        render(LayoutControls, {
            props: {
                libraryOpen: true,
                editorOpen: true,
                editorAvailable: false,
                inspectorOpen: true,
                ontogglelibrary: vi.fn(),
                ontoggleeditor: vi.fn(),
                ontoggleinspector: vi.fn(),
            },
        });

        expect(screen.getByRole('button', { name: 'Editor panel' }).hasAttribute('disabled')).toBe(true);
    });
});
