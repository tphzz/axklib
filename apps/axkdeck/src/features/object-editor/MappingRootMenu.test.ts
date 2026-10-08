import { fireEvent, render } from '@testing-library/svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';
import MappingRootMenu from './MappingRootMenu.svelte';

afterEach(() => {
    vi.restoreAllMocks();
    vi.unstubAllGlobals();
});

describe('mapping root menu placement', () => {
    it.each([1, 1.25, 1.5])(
        'clamps viewport coordinates before converting to CSS offsets at scale %s',
        async (scale) => {
            vi.stubGlobal('innerWidth', 800);
            vi.stubGlobal('innerHeight', 600);
            vi.spyOn(HTMLElement.prototype, 'offsetWidth', 'get').mockReturnValue(160);
            vi.spyOn(HTMLElement.prototype, 'getBoundingClientRect').mockReturnValue({
                width: 160 * scale,
                height: 32 * scale,
            } as DOMRect);
            const onclose = vi.fn();
            const view = render(MappingRootMenu, {
                note: 60,
                x: 799,
                y: 599,
                formatNote: () => 'C3',
                onchange: vi.fn(),
                onclose,
            });
            const menu = view.getByRole('menu');
            expect(parseFloat(menu.style.left) * scale + 160 * scale).toBeCloseTo(792);
            expect(parseFloat(menu.style.top) * scale + 32 * scale).toBeCloseTo(592);
            await fireEvent(window, new Event('resize'));
            expect(onclose).toHaveBeenCalledOnce();
        },
    );
});
