export async function revealAfterInterfaceScale(
    interfaceScaleReady: Promise<unknown>,
    waitForFirstFrame: () => Promise<void>,
    root: HTMLElement = document.documentElement,
): Promise<void> {
    await interfaceScaleReady;
    root.removeAttribute('data-interface-scale-pending');
    await waitForFirstFrame();
}
