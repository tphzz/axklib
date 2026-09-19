import { render, fireEvent } from '@testing-library/svelte';
import { beforeAll, afterAll, describe, expect, it, vi } from 'vitest';
import EditorChoice from './EditorChoice.svelte';

const options = Array.from({ length: 12 }, (_, value) => ({ value, label: `Option ${value}` }));
const originalScroll = Element.prototype.scrollIntoView;
beforeAll(() => {
    Element.prototype.scrollIntoView = vi.fn();
});
afterAll(() => {
    Element.prototype.scrollIntoView = originalScroll;
});
describe('editor choices', () => {
    it('opens only deliberately and supports selection, Escape and outside dismissal', async () => {
        const onchange = vi.fn();
        const view = render(EditorChoice, { label: 'Output', value: 3, options, onchange });
        const trigger = view.getByRole('button');
        await fireEvent.focus(trigger);
        expect(view.queryByRole('listbox')).toBeNull();
        await fireEvent.keyDown(trigger, { key: 'ArrowDown' });
        let popup = view.getByRole('listbox');
        expect(popup.parentElement).toBe(document.body);
        await fireEvent.keyDown(popup, { key: 'End' });
        await fireEvent.keyDown(popup, { key: 'Enter' });
        expect(onchange).toHaveBeenCalledWith(11);
        expect(view.queryByRole('listbox')).toBeNull();
        await fireEvent.click(trigger);
        popup = view.getByRole('listbox');
        await fireEvent.pointerDown(popup);
        expect(view.queryByRole('listbox')).not.toBeNull();
        await fireEvent.keyDown(popup, { key: 'Escape' });
        expect(view.queryByRole('listbox')).toBeNull();
        expect(document.activeElement).toBe(trigger);
        await fireEvent.click(trigger);
        await fireEvent.pointerDown(document.body);
        expect(view.queryByRole('listbox')).toBeNull();
    });
    it('uses arrow keys for segmented playback choices', async () => {
        const onchange = vi.fn();
        const view = render(EditorChoice, {
            label: 'Playback',
            value: 0,
            options: options.slice(0, 6),
            segmented: true,
            onchange,
        });
        await fireEvent.keyDown(view.getByRole('button', { name: 'Playback: Option 0' }), { key: 'ArrowLeft' });
        expect(onchange).toHaveBeenCalledWith(5);
    });
    it('ignores queued scroll notifications at the same anchor but closes when the anchor moves', async () => {
        const view = render(EditorChoice, { label: 'Mode', value: 0, options, onchange: vi.fn() });
        const trigger = view.getByRole('button');
        const bounds = vi
            .spyOn(trigger, 'getBoundingClientRect')
            .mockReturnValue({ top: 0, left: 0, width: 100, bottom: 26, height: 26 } as DOMRect);
        await fireEvent.click(trigger);
        await fireEvent.scroll(window);
        expect(view.queryByRole('listbox')).not.toBeNull();
        bounds.mockReturnValue({ top: 20, left: 0, width: 100, bottom: 46, height: 26 } as DOMRect);
        await fireEvent.scroll(window);
        expect(view.queryByRole('listbox')).toBeNull();
    });
});
