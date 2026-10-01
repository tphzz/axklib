<script lang="ts">
    import { sampleTabs } from './fields';
    import EditorTabs from '../../../../lib/components/EditorTabs.svelte';
    import type { EditorNavigation } from '../../../object-editor/navigation.svelte';
    let { navigation, panelId }: { navigation: EditorNavigation; panelId: string } = $props();
</script>

<EditorTabs
    tabs={sampleTabs}
    active={navigation.tab}
    onselect={(id) => navigation.selectTab(id)}
    label="Sample parameter tabs"
    idPrefix={panelId}
    {panelId}
/>
