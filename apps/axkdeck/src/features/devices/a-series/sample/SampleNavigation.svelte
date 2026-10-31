<script lang="ts">
    import { sampleTabs } from './fields';
    import TabStrip from '../../../../lib/components/TabStrip.svelte';
    import type { EditorNavigation } from '../../../object-editor/navigation.svelte';
    let { navigation, panelId }: { navigation: EditorNavigation; panelId: string } = $props();
</script>

<TabStrip
    tabs={sampleTabs}
    active={navigation.tab}
    onselect={(id) => navigation.selectTab(id)}
    label="Sample parameter tabs"
    idPrefix={panelId}
    {panelId}
/>
