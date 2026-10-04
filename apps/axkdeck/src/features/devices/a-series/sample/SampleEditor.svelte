<script lang="ts">
    import { sampleSnapshot } from '../../../../lib/objectEditing';
    import { setContext, untrack } from 'svelte';
    import { sampleFormatContext } from './formatCapabilities';
    import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';
    import type { EditorNavigation } from '../../../object-editor/navigation.svelte';
    import { editorScrollContext, rememberEditorScroll } from '../../../object-editor/editorScroll';
    import type { SampleWaveformPreview } from '../../../../lib/types';
    import { sampleTabs } from './fields';
    import TrimLoop from './TrimLoop.svelte';
    import SampleInfo from './SampleInfo.svelte';
    import SampleTransport from './SampleTransport.svelte';
    import ParameterPage from './ParameterPage.svelte';
    import EditorHeader from '../../../../lib/components/EditorHeader.svelte';
    import EditorTabs from '../../../../lib/components/EditorTabs.svelte';
    let {
        document: suppliedDocument,
        preview,
        panelId,
        navigation,
        inactive = false,
    }: {
        document: ObjectEditorDocument;
        preview?: SampleWaveformPreview;
        panelId: string;
        navigation: EditorNavigation;
        inactive?: boolean;
    } = $props();
    // The host keys this component by document; cleanup must retain that identity.
    const document = untrack(() => suppliedDocument);
    setContext(sampleFormatContext, () => sampleSnapshot(document.detail)!);
    const bank = $derived(sampleSnapshot(document.detail)!.profile === 'a-series/sample-bank');
    const tabs = $derived(
        bank
            ? sampleTabs.map((tab) => ({ ...tab, pages: tab.pages.filter((page) => page.id !== 'waveform') }))
            : sampleTabs,
    );
    const activeTab = $derived(tabs.find((tab) => tab.id === navigation.tab) ?? tabs[0]!);
    const activePage = $derived(activeTab.pages.find((page) => page.id === navigation.page) ?? activeTab.pages[0]!);
    const scroll = $derived({ positions: navigation.scrollPositions, key: activePage.id });
    setContext(editorScrollContext, () => scroll);
    const disabled = $derived(
        inactive || document.phase !== 'editable' || !sampleSnapshot(document.detail)?.editable || !!document.conflict,
    );
    const rate = $derived(
        bank
            ? (sampleSnapshot(document.previewDetail)?.sources[0]?.sampleRate ?? 44100)
            : (preview?.preview?.lanes[0]?.sampleRate ?? 44100),
    );
    let transport: SampleTransport;
</script>

<div class="sample-pages" role="group" aria-label={`${activeTab.label} pages`}>
    <EditorHeader secondary>
        <EditorTabs
            secondary
            tabs={activeTab.pages}
            active={activePage.id}
            onselect={(id) => (navigation.page = id)}
            label="Sample subpages"
        />
        {#snippet tools()}
            <span class="editor-meta"
                >{activeTab.label} · {activeTab.pages.indexOf(activePage) + 1} / {activeTab.pages.length}</span
            >
        {/snippet}
    </EditorHeader>
</div>
<div
    role="tabpanel"
    id={panelId}
    aria-labelledby={`${panelId}-${activeTab.id}`}
    class="sample-panel"
    use:rememberEditorScroll={scroll}
>
    {#if activePage.id === 'waveform'}
        <TrimLoop {document} {preview} {disabled} onseek={(frame) => transport?.seek(frame)} />
    {:else if activePage.id === 'sample-info' && bank}
        <ParameterPage
            {document}
            page={{
                ...activePage,
                fields: activePage.fields.filter((field) => field.key === 'wave_start_velocity_sensitivity'),
            }}
            {disabled}
        />
    {:else if activePage.id === 'sample-info'}
        <SampleInfo {document} {rate} {disabled} onmonitor={() => void transport?.play(true)} />
    {:else}
        <ParameterPage {document} page={activePage} {disabled} />
    {/if}
</div>
<SampleTransport
    bind:this={transport}
    {document}
    {rate}
    disabled={inactive || document.phase !== 'editable' || !!document.conflict}
/>

<style>
    .sample-pages {
        flex: 0 0 auto;
        min-width: 0;
    }
    .sample-panel {
        flex: 1;
        min-height: 0;
        overflow: auto;
        padding: var(--density-panel-padding, 6px);
    }
    :global([data-editor-under~='650']) .editor-meta {
        display: none;
    }
    :global([data-editor-under~='650']) .sample-panel {
        padding: 8px;
    }
</style>
