import { fireEvent, render } from '@testing-library/svelte';
import { expect, it, vi } from 'vitest';
import Splitter from './Splitter.svelte';

it.each(['horizontal', 'vertical'] as const)('shares %s keyboard and pointer behavior', async (orientation) => {
    const resize = vi.fn(),
        step = vi.fn(),
        reset = vi.fn();
    const view = render(Splitter, {
        orientation,
        label: 'Resize pane',
        value: 50,
        onresize: resize,
        onstep: step,
        onreset: reset,
    });
    const separator = view.getByRole('separator', { name: 'Resize pane' });
    expect(separator.getAttribute('aria-orientation')).toBe(orientation);
    separator.setPointerCapture = vi.fn();
    separator.releasePointerCapture = vi.fn();
    await fireEvent.keyDown(separator, { key: 'Home' });
    await fireEvent.keyDown(separator, { key: 'End' });
    expect(step.mock.calls).toEqual([['Home'], ['End']]);
    await fireEvent.pointerDown(separator, { pointerId: 3, button: 0 });
    await fireEvent.pointerMove(separator, { pointerId: 3 });
    await fireEvent.pointerUp(separator, { pointerId: 3 });
    expect(resize).toHaveBeenCalledTimes(3);
    await fireEvent.pointerMove(separator, { pointerId: 3 });
    expect(resize).toHaveBeenCalledTimes(3);
    await fireEvent.doubleClick(separator);
    expect(reset).toHaveBeenCalledOnce();
});
