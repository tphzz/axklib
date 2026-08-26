import { describe, expect, it, vi } from 'vitest';

import { revealAfterInterfaceScale, waitForVisibleScaleCommit } from './startupVisibility';

describe('startup visibility', () => {
    it('keeps the shell hidden until the scaled native window has committed a frame', async () => {
        document.documentElement.setAttribute('data-interface-scale-pending', '');
        let finishScale: (() => void) | undefined;
        let finishScaleCommit: (() => void) | undefined;
        let finishVisibleFrame: (() => void) | undefined;
        const scaleReady = new Promise<void>((resolve) => {
            finishScale = resolve;
        });
        const waitForScaleCommit = vi.fn(
            () =>
                new Promise<void>((resolve) => {
                    finishScaleCommit = resolve;
                }),
        );
        const waitForFirstVisibleFrame = vi.fn(
            () =>
                new Promise<void>((resolve) => {
                    finishVisibleFrame = resolve;
                }),
        );
        const showWindow = vi.fn(async () => undefined);

        const ready = revealAfterInterfaceScale(scaleReady, waitForFirstVisibleFrame, {
            root: document.documentElement,
            showWindow,
            waitForScaleCommit,
        });

        expect(document.documentElement.hasAttribute('data-interface-scale-pending')).toBe(true);
        expect(waitForFirstVisibleFrame).not.toHaveBeenCalled();
        expect(showWindow).not.toHaveBeenCalled();

        finishScale?.();
        await vi.waitFor(() => expect(showWindow).toHaveBeenCalledOnce());

        expect(document.documentElement.hasAttribute('data-interface-scale-pending')).toBe(true);
        expect(waitForScaleCommit).toHaveBeenCalledOnce();
        expect(waitForFirstVisibleFrame).not.toHaveBeenCalled();

        finishScaleCommit?.();
        await vi.waitFor(() => expect(waitForFirstVisibleFrame).toHaveBeenCalledOnce());

        expect(document.documentElement.hasAttribute('data-interface-scale-pending')).toBe(false);
        expect(waitForFirstVisibleFrame).toHaveBeenCalledOnce();

        finishVisibleFrame?.();
        await ready;

        expect(document.documentElement.hasAttribute('data-interface-scale-pending')).toBe(false);
        expect(waitForFirstVisibleFrame).toHaveBeenCalledOnce();
        expect(showWindow).toHaveBeenCalledOnce();
    });

    it('uses two animation frames as the visible scale commit barrier', async () => {
        const frames: FrameRequestCallback[] = [];
        let completed = false;
        const ready = waitForVisibleScaleCommit((callback) => frames.push(callback)).then(() => {
            completed = true;
        });

        expect(frames).toHaveLength(1);
        frames.shift()?.(0);
        expect(completed).toBe(false);
        expect(frames).toHaveLength(1);

        frames.shift()?.(16);
        await ready;
        expect(completed).toBe(true);
    });
});
