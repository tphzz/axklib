import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import LayoutControls from './LayoutControls.svelte';
import type { InterfaceScaleState } from '../interfaceScale';

const autoScale: InterfaceScaleState = { mode: 'auto', appliedZoom: 1.5 };

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

    it('offers a keyboard-navigable interface-scale menu and reports fixed selections', async () => {
        const oninterfacescalechange = vi.fn();
        render(LayoutControls, {
            props: {
                libraryOpen: true,
                editorOpen: true,
                editorAvailable: true,
                inspectorOpen: true,
                interfaceScale: autoScale,
                ontogglelibrary: vi.fn(),
                ontoggleeditor: vi.fn(),
                ontoggleinspector: vi.fn(),
                oninterfacescalechange,
            },
        });

        const trigger = screen.getByRole('button', { name: 'Interface scale, Auto, 150%' });
        expect(trigger.getAttribute('aria-expanded')).toBe('false');
        await fireEvent.keyDown(trigger, { key: 'ArrowDown' });
        expect(trigger.getAttribute('aria-expanded')).toBe('true');
        expect(screen.getByRole('menuitemradio', { name: 'Auto' }).getAttribute('aria-checked')).toBe('true');

        const automatic = screen.getByRole('menuitemradio', { name: 'Auto' });
        expect(document.activeElement).toBe(automatic);
        await fireEvent.keyDown(automatic, { key: 'ArrowDown' });
        expect(document.activeElement).toBe(screen.getByRole('menuitemradio', { name: '100%' }));
        await fireEvent.keyDown(document.activeElement!, { key: 'Escape' });
        expect(screen.queryByRole('menu', { name: 'Interface scale' })).toBeNull();
        expect(document.activeElement).toBe(trigger);

        await fireEvent.click(trigger);
        await fireEvent.click(screen.getByRole('menuitemradio', { name: '125%' }));

        expect(oninterfacescalechange).toHaveBeenCalledWith('1.25');
        expect(screen.queryByRole('menu', { name: 'Interface scale' })).toBeNull();
    });

    it('closes the scale menu with Escape and outside clicks', async () => {
        render(LayoutControls, {
            props: {
                libraryOpen: true,
                editorOpen: true,
                editorAvailable: true,
                inspectorOpen: true,
                interfaceScale: autoScale,
                ontogglelibrary: vi.fn(),
                ontoggleeditor: vi.fn(),
                ontoggleinspector: vi.fn(),
                oninterfacescalechange: vi.fn(),
            },
        });

        const trigger = screen.getByRole('button', { name: 'Interface scale, Auto, 150%' });
        await fireEvent.click(trigger);
        await fireEvent.keyDown(window, { key: 'Escape' });
        expect(screen.queryByRole('menu', { name: 'Interface scale' })).toBeNull();

        await fireEvent.click(trigger);
        await fireEvent.click(window);
        expect(screen.queryByRole('menu', { name: 'Interface scale' })).toBeNull();
    });

    it('labels a fixed scale once without presenting it as an automatic result', async () => {
        render(LayoutControls, {
            props: {
                libraryOpen: true,
                editorOpen: true,
                editorAvailable: true,
                inspectorOpen: true,
                interfaceScale: { mode: '1.25', appliedZoom: 1.25 },
                ontogglelibrary: vi.fn(),
                ontoggleeditor: vi.fn(),
                ontoggleinspector: vi.fn(),
                oninterfacescalechange: vi.fn(),
            },
        });

        const trigger = screen.getByRole('button', { name: 'Interface scale, 125%' });
        await fireEvent.click(trigger);
        expect(screen.getByRole('menuitemradio', { name: 'Auto' }).getAttribute('aria-checked')).toBe('false');
        expect(screen.getByRole('menuitemradio', { name: '125%' }).getAttribute('aria-checked')).toBe('true');
    });
});
