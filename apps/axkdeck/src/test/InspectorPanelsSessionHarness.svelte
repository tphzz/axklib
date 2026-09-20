<script lang="ts">
    import FilesInspector from '../features/files/FilesInspector.svelte';
    import ObjectInspector from '../lib/components/ObjectInspector.svelte';
    import type { FilesystemEntry } from '../lib/filesystem';
    import { provideInspectorPanels } from '../lib/inspectorPanels.svelte';
    import type { InspectorSelection } from '../lib/types';

    let { selection, mode }: { selection: InspectorSelection; mode: 'object' | 'files' | null } = $props();
    provideInspectorPanels();

    const entry: FilesystemEntry = {
        id: 'entry-first',
        parentId: 'root',
        rootId: 'root',
        ancestorIds: ['root'],
        name: 'First sample',
        path: '/First sample',
        kind: 'file',
        sizeBytes: 356,
        childCount: 0,
        objectId: 'sample-first',
        contentScopeId: null,
        interpretation: '',
        storage: 'Record 0',
        issue: '',
        filesystemMetadata: false,
        rawAttributes: 'SFS 0x94000000',
        attributes: [],
    };
</script>

{#if mode === 'object'}
    <ObjectInspector {selection} />
{:else if mode === 'files'}
    <FilesInspector {entry} />
{/if}
