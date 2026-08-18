import type { WorkspaceView } from '../../lib/types';

export interface WorkspaceTab {
    id: WorkspaceView;
    label: string;
    icon: 'music' | 'layers' | 'archive' | 'waveform' | 'list';
}

export const workspaceTabs: WorkspaceTab[] = [
    { id: 'programs', label: 'Programs', icon: 'music' },
    { id: 'sample-banks', label: 'Sample Banks', icon: 'layers' },
    { id: 'samples', label: 'Samples', icon: 'archive' },
    { id: 'wave-data', label: 'Wave Data', icon: 'waveform' },
    { id: 'sequences', label: 'Sequences', icon: 'list' },
];
