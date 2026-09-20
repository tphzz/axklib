<script lang="ts">
    import { setContext, untrack } from 'svelte';
    import { sampleFormatContext } from './formatCapabilities';
    import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';
    import type { EditorNavigation } from '../../../object-editor/navigation.svelte';
    import type { SampleWaveformPreview } from '../../../../lib/types';
    import { sampleTabs } from './fields';
    import TrimLoop from './TrimLoop.svelte';
    import SampleInfo from './SampleInfo.svelte';
    import SampleTransport from './SampleTransport.svelte';
    import ParameterPage from './ParameterPage.svelte';
    let {
        document: suppliedDocument,
        preview,
        panelId,
        navigation,
    }: {
        document: ObjectEditorDocument;
        preview: SampleWaveformPreview;
        panelId: string;
        navigation: EditorNavigation;
    } = $props();
    // The host keys this component by document; cleanup must retain that identity.
    const document = untrack(() => suppliedDocument);
    setContext(sampleFormatContext, () => document.detail!.editing!);
    const activeTab = $derived(sampleTabs.find((tab) => tab.id === navigation.tab) ?? sampleTabs[0]!);
    const activePage = $derived(activeTab.pages.find((page) => page.id === navigation.page) ?? activeTab.pages[0]!);
    const disabled = $derived(
        document.phase !== 'editable' || !document.detail?.editing?.editable || !!document.conflict,
    );
    const rate = $derived(preview.preview?.lanes[0]?.sampleRate ?? 44100);
    let transport: SampleTransport;
</script>

<div class="sample-pages" role="group" aria-label={`${activeTab.label} pages`}>
    <nav aria-label="Sample subpages">
        {#each activeTab.pages as page, index}
            <button
                aria-pressed={activePage.id === page.id}
                onclick={() => (navigation.page = page.id)}
                onkeydown={(event) => {
                    let next = index;
                    if (event.key === 'ArrowRight') next = (index + 1) % activeTab.pages.length;
                    else if (event.key === 'ArrowLeft')
                        next = (index + activeTab.pages.length - 1) % activeTab.pages.length;
                    else if (event.key === 'Home') next = 0;
                    else if (event.key === 'End') next = activeTab.pages.length - 1;
                    else return;
                    event.preventDefault();
                    navigation.page = activeTab.pages[next]!.id;
                    (event.currentTarget.parentElement?.children[next] as HTMLButtonElement)?.focus();
                }}>{page.label}</button
            >
        {/each}
    </nav>
    <span class="editor-meta"
        >{activeTab.label} · {activeTab.pages.indexOf(activePage) + 1} / {activeTab.pages.length}</span
    >
</div>
<div role="tabpanel" id={panelId} aria-labelledby={`${panelId}-${activeTab.id}`} class="sample-panel">
    {#if activePage.id === 'waveform'}
        <TrimLoop {document} {preview} {disabled} onseek={(frame) => transport?.seek(frame)} />
    {:else if activePage.id === 'sample-info'}
        <SampleInfo {document} {rate} {disabled} onmonitor={() => void transport?.play(true)} />
    {:else}
        <ParameterPage {document} page={activePage} {disabled} />
    {/if}
</div>
<SampleTransport bind:this={transport} {document} {rate} {disabled} />

<style>
    .sample-pages {
        display: flex;
        align-items: center;
        gap: 12px;
        flex: 0 0 30px;
        min-height: 30px;
        padding: 0 8px;
        border-bottom: 1px solid var(--color-border);
    }
    nav {
        display: flex;
        align-items: center;
        gap: 3px;
        min-width: 0;
        overflow-x: auto;
        scrollbar-width: none;
    }
    nav::-webkit-scrollbar {
        display: none;
    }
    button {
        border: 1px solid transparent;
        background: transparent;
        padding: 3px 9px;
        white-space: nowrap;
        color: var(--color-text-muted);
        font-size: 11px;
        border-radius: 3px;
    }
    button[aria-pressed='true'] {
        border-color: var(--color-accent);
        color: var(--color-text);
        background: var(--color-panel-raised);
    }
    button:focus-visible {
        outline: 1px solid var(--color-accent);
    }
    .sample-pages > span {
        margin-left: auto;
        white-space: nowrap;
    }
    .sample-panel {
        flex: 1;
        min-height: 0;
        overflow: auto;
        padding: var(--density-panel-padding, 6px);
    }
    :global([data-editor-under~='650']) .sample-pages > span {
        display: none;
    }
    :global([data-editor-under~='650']) .sample-panel {
        padding: 8px;
    }
</style>
