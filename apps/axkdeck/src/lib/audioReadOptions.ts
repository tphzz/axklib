export interface PreviewReadOptions {
    signal?: AbortSignal;
    readIntent: 'SELECTED' | 'REFINEMENT';
}

export interface AuditionReadOptions {
    signal?: AbortSignal;
    sourceWindow?: 'PLAYBACK' | 'STORED';
    readIntent: 'PLAYBACK' | 'PREFETCH';
}

export interface AuditionContentOptions {
    signal?: AbortSignal;
    readIntent: 'PLAYBACK' | 'PREFETCH';
}
