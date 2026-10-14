import type { Component, Snippet } from 'svelte';
import type { AppProps } from '../../appProps';

export type WorkspaceMode = 'device' | 'files';

// All four zones belong to the active presentation, not to a sampler-specific shell.
export interface WorkspacePresentation {
    navigation: Snippet;
    content: Snippet;
    inspector?: Snippet;
    lower?: Snippet;
    lowerPreferredHeight?: number;
    tabs?: Snippet;
    playback?: Snippet;
    selectionActions?: Snippet;
}

export interface DevicePresentationState {
    lowerOpen: boolean;
    setLowerOpen: (value: boolean) => void;
    showObject: (objectId: string) => Promise<void>;
}

export interface WorkspaceBackend {
    id: string;
    label: string;
    application: Component<AppProps>;
}
