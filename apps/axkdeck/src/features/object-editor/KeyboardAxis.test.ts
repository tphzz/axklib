import { fireEvent, render } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import KeyboardAxis from './KeyboardAxis.svelte';

const zone = { id: 'a', label: 'A', low: 24, high: 60, velocityLow: 0, velocityHigh: 127, selected: true };
describe('mapping keyboard audition', () => {
    it('uses the same muted surface blend as the selected mapping, with a separate root marker', () => {
        const view = render(KeyboardAxis, { zones: [zone], roots: [36], formatNote: String });
        expect(view.container.querySelector('[data-note="24"]')?.getAttribute('fill')).toBe(
            'color-mix(in srgb, var(--editor-loop) 50%, var(--color-panel-deep))',
        );
        expect(view.container.querySelector('[data-note="36"]')?.getAttribute('fill')).toBe(
            'color-mix(in srgb, var(--editor-loop) 50%, var(--color-panel-deep))',
        );
        expect(view.container.querySelector('.root-marker')).not.toBeNull();
    });
    it('auditions a held key without selecting a mapping or changing its root', async () => {
        const onpress = vi.fn(),
            onrelease = vi.fn(),
            onrootmenu = vi.fn();
        const view = render(KeyboardAxis, { zones: [zone], formatNote: String, onpress, onrelease, onrootmenu });
        const key = view.getByRole('button', { name: '36' });
        key.setPointerCapture = vi.fn();
        key.hasPointerCapture = () => false;
        await fireEvent.pointerDown(key, { button: 0, pointerId: 1 });
        expect(onpress).toHaveBeenCalledExactlyOnceWith(36);
        await fireEvent.pointerUp(key, { pointerId: 1 });
        await fireEvent.click(key);
        expect(onrelease).toHaveBeenCalledTimes(1);
        expect(onrootmenu).not.toHaveBeenCalled();
        expect(onpress).toHaveBeenCalledTimes(1);
    });
    it('supports held keyboard activation and releases on blur without repeating notes', async () => {
        const onpress = vi.fn(),
            onrelease = vi.fn();
        const view = render(KeyboardAxis, { zones: [zone], formatNote: String, onpress, onrelease });
        const key = view.getByRole('button', { name: '36' });
        await fireEvent.keyDown(key, { key: ' ', repeat: false });
        await fireEvent.keyDown(key, { key: ' ', repeat: true });
        expect(onpress).toHaveBeenCalledExactlyOnceWith(36);
        await fireEvent.blur(key);
        await fireEvent.keyUp(key, { key: ' ' });
        expect(onrelease).toHaveBeenCalledTimes(1);
    });
    it('retains key nodes through viewport changes while excluding clipped keys from navigation', async () => {
        const view = render(KeyboardAxis, { zones: [zone], formatNote: String, onpress: vi.fn() });
        const clipped = view.getByRole('button', { name: '0' });
        const visible = view.getByRole('button', { name: '36' });
        await view.rerender({ start: 24, end: 60 });
        expect(view.queryByRole('button', { name: '0' })).toBeNull();
        expect(view.container.querySelector('[data-key="0"]')).toBe(clipped);
        expect(view.getByRole('button', { name: '36' })).toBe(visible);
        expect(clipped.getAttribute('tabindex')).toBe('-1');
        await view.rerender({ start: 0, end: 127 });
        expect(view.getByRole('button', { name: '0' })).toBe(clipped);
    });
});
