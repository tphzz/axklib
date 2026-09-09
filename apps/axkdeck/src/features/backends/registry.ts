import Application from './axklib/Application.svelte';
import type { WorkspaceBackend } from '../workspace/contracts';

export const workspaceBackends: readonly WorkspaceBackend[] = [
    { id: 'axklib', label: 'axklib', application: Application },
];
