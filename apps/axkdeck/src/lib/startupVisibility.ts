interface StartupRevealOptions {
    root?: HTMLElement;
    showWindow?: () => Promise<void>;
    waitForScaleCommit?: () => Promise<void>;
}

type FrameRequester = (callback: FrameRequestCallback) => number;

export function waitForVisibleScaleCommit(
    requestFrame: FrameRequester = window.requestAnimationFrame.bind(window),
): Promise<void> {
    return new Promise<void>((resolve) => requestFrame(() => requestFrame(() => resolve())));
}

export async function revealAfterInterfaceScale(
    interfaceScaleReady: Promise<unknown>,
    waitForFirstVisibleFrame: () => Promise<void>,
    options: StartupRevealOptions = {},
): Promise<void> {
    const root = options.root ?? document.documentElement;
    await interfaceScaleReady;
    await options.showWindow?.();
    await options.waitForScaleCommit?.();
    root.removeAttribute('data-interface-scale-pending');
    await waitForFirstVisibleFrame();
}
