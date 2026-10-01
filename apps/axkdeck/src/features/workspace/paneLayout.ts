export function editorPaneHeight(available: number, upperRatio: number | null, preferred = 180): number {
    const minimum = Math.min(180, available / 2);
    const wanted = upperRatio === null ? Math.max(available / 3, preferred) : available * (1 - upperRatio);
    return Math.max(minimum, Math.min(available - minimum, wanted));
}
