// Resolution can change when moving between monitors without a CSS resize.
export function observeCanvas(node: HTMLCanvasElement, changed: () => void): () => void {
    let query: MediaQueryList | undefined;
    const resolutionChanged = () => {
        query?.removeEventListener('change', resolutionChanged);
        query = window.matchMedia?.(`(resolution: ${window.devicePixelRatio || 1}dppx)`);
        query?.addEventListener('change', resolutionChanged);
        changed();
    };
    const observer = new ResizeObserver(changed);
    observer.observe(node);
    resolutionChanged();
    return () => {
        observer.disconnect();
        query?.removeEventListener('change', resolutionChanged);
    };
}
