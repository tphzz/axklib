export interface CatalogHooks {
    stopPlayback: () => Promise<void>;
    resetPreviews: () => void;
    resetCleanup: () => void;
}

export function createCatalogHooks(): CatalogHooks {
    return {
        stopPlayback: () => Promise.resolve(),
        resetPreviews: () => {},
        resetCleanup: () => {},
    };
}
