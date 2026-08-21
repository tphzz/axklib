import { describe, expect, it, vi } from 'vitest';

import { revealAfterInterfaceScale } from './startupVisibility';

describe('startup visibility', () => {
    it('reveals the mounted shell before measuring its first visible frame', async () => {
        document.documentElement.setAttribute('data-interface-scale-pending', '');
        let finishScale: (() => void) | undefined;
        const scaleReady = new Promise<void>((resolve) => {
            finishScale = resolve;
        });
        const waitForFirstFrame = vi.fn(async () => undefined);

        const ready = revealAfterInterfaceScale(scaleReady, waitForFirstFrame);

        expect(document.documentElement.hasAttribute('data-interface-scale-pending')).toBe(true);
        expect(waitForFirstFrame).not.toHaveBeenCalled();

        finishScale?.();
        await ready;

        expect(document.documentElement.hasAttribute('data-interface-scale-pending')).toBe(false);
        expect(waitForFirstFrame).toHaveBeenCalledOnce();
    });
});
