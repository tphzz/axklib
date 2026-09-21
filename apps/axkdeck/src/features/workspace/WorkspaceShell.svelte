<script lang="ts">
    import { onDestroy, onMount, type Snippet } from 'svelte';
    import Icon from '../../lib/components/Icon.svelte';
    import LayoutControls from '../../lib/components/LayoutControls.svelte';
    import { useASeriesPreferences } from '../../lib/aSeriesPreferences.svelte';
    import type { InterfaceScaleController, InterfaceScaleState } from '../../lib/interfaceScale';
    import type { WorkspaceMode, WorkspacePresentation } from './contracts';
    import { objectEditors } from '../object-editor/context';
    import { editorPaneHeight } from './paneLayout';
    import Splitter from '../../lib/components/Splitter.svelte';

    interface Props {
        mode: WorkspaceMode;
        presentation: WorkspacePresentation;
        imageActions: Snippet;
        imageName?: string;
        context?: string;
        status?: string;
        count?: string;
        deviceAvailable?: boolean;
        filesAvailable?: boolean;
        inspectorOpen?: boolean;
        lowerOpen?: boolean;
        interfaceScaling?: InterfaceScaleController | null;
        isDesktop?: boolean;
        onmodechange: (mode: WorkspaceMode) => void;
        onabout?: () => void;
        onconnection?: () => void;
    }
    let {
        mode,
        presentation,
        imageActions,
        imageName = '',
        context = '',
        status = '',
        count = '',
        deviceAvailable = true,
        filesAvailable = true,
        inspectorOpen = $bindable(true),
        lowerOpen = $bindable(false),
        interfaceScaling = null,
        isDesktop = false,
        onmodechange,
        onabout = () => undefined,
        onconnection = () => undefined,
    }: Props = $props();
    let sidebarOpen = $state(true);
    let splitRatio = $state<number | null>(null);
    let stageHeight = $state(0);
    let mainStage: HTMLElement;
    let scale = $state<InterfaceScaleState | null>(null);
    let unsubscribe: (() => void) | undefined;
    const preferences = useASeriesPreferences();
    let preferencesOpen = $state(false);
    let preferencesReady = $state(false);
    const editors = objectEditors();
    const playback = $derived(editors?.visible ? undefined : presentation.playback);
    const availableHeight = $derived(Math.max(0, stageHeight - 8 - (playback ? 30 : 0)));
    const lowerHeight = $derived(editorPaneHeight(availableHeight, splitRatio, presentation.lowerPreferredHeight));
    const upperHeight = $derived(availableHeight - lowerHeight);
    onMount(() => {
        void preferences.ready.then(() => {
            preferencesReady = true;
        });
        scale = interfaceScaling?.state() ?? null;
        unsubscribe = interfaceScaling?.subscribe((value) => {
            scale = value;
        });
        const observer = new ResizeObserver(() => (stageHeight = mainStage.clientHeight));
        observer.observe(mainStage);
        stageHeight = mainStage.clientHeight;
        return () => observer.disconnect();
    });
    onDestroy(() => {
        unsubscribe?.();
        void interfaceScaling?.dispose();
    });
    function resize(clientY: number): void {
        const bounds = mainStage.getBoundingClientRect();
        const logicalY = ((clientY - bounds.top) / Math.max(1, bounds.height)) * stageHeight;
        splitRatio = Math.max(0, Math.min(1, (logicalY - (playback ? 30 : 0)) / Math.max(1, availableHeight)));
    }
    function resizeKey(key: string): void {
        splitRatio = Math.max(
            0,
            Math.min(
                1,
                key === 'Home'
                    ? 0
                    : key === 'End'
                      ? 1
                      : upperHeight / Math.max(1, availableHeight) + (key === 'ArrowDown' ? 0.03 : -0.03),
            ),
        );
    }
</script>

<div
    class="app-shell modular-shell"
    class:sidebar-closed={!sidebarOpen}
    class:inspector-closed={!inspectorOpen || !presentation.inspector}
    data-workspace-mode={mode}
>
    <header class="app-header">
        <button class="brand" type="button" aria-label="About axkdeck" title="About axkdeck" onclick={onabout}>
            <span class="brand-mark"><Icon name="waveform" size={20} /></span><strong>axkdeck</strong>
        </button>
        <div class="workspace-image-context">
            <strong title={imageName}>{imageName}</strong><small>{context}</small>
        </div>
        <div class="global-controls">
            {#if isDesktop}<button
                    class="icon-button"
                    type="button"
                    title="Server connection settings"
                    aria-label="Server connection settings"
                    onclick={onconnection}><Icon name="server" size={17} /></button
                >{/if}
            <button
                class="icon-button"
                type="button"
                aria-label="Preferences"
                title="Preferences"
                disabled={!preferencesReady}
                onclick={() => (preferencesOpen = true)}><Icon name="settings" size={17} /></button
            >
            <LayoutControls
                libraryOpen={sidebarOpen}
                editorOpen={lowerOpen && Boolean(presentation.lower)}
                editorAvailable={Boolean(presentation.lower)}
                {inspectorOpen}
                interfaceScale={scale}
                ontogglelibrary={() => (sidebarOpen = !sidebarOpen)}
                ontoggleeditor={() => (lowerOpen = !lowerOpen)}
                ontoggleinspector={() => (inspectorOpen = !inspectorOpen)}
                oninterfacescalechange={(value) => void interfaceScaling?.setMode(value)}
            />
        </div>
        <div class="workspace-mode-switch" role="group" aria-label="Workspace mode">
            <button
                type="button"
                class:active={mode === 'device'}
                aria-pressed={mode === 'device'}
                disabled={!deviceAvailable}
                title={deviceAvailable ? 'Device view' : 'No supported device view for this image'}
                onclick={() => onmodechange('device')}><Icon name="hard-drive" size={16} /> Device</button
            >
            <button
                type="button"
                class:active={mode === 'files'}
                aria-pressed={mode === 'files'}
                disabled={!filesAvailable}
                title={filesAvailable ? 'Filesystem view' : 'No filesystem view for this source'}
                onclick={() => onmodechange('files')}><Icon name="folder" size={16} /> Files</button
            >
        </div>
    </header>
    {#if sidebarOpen}
        <aside class="image-navigator" aria-label="Image navigator" data-workspace-background>
            {@render imageActions()}
            {@render presentation.navigation()}
        </aside>
    {/if}
    <div class="workspace-center" data-workspace-background>
        {#if presentation.tabs || presentation.selectionActions}
            <div class="workspace-view-toolbar">
                {#if presentation.tabs}{@render presentation.tabs()}{/if}
                {#if presentation.selectionActions}{@render presentation.selectionActions()}{/if}
            </div>
        {/if}
        <main
            bind:this={mainStage}
            class="main-stage"
            data-workspace-background
            class:lower-panel-closed={!lowerOpen || !presentation.lower}
            class:has-audition-bar={Boolean(playback)}
            style:--split-position={`${upperHeight}px`}
        >
            {@render presentation.content()}
            {#if playback}{@render playback()}{/if}
            {#if lowerOpen && presentation.lower}
                <Splitter
                    orientation="horizontal"
                    label="Resize editor panel"
                    value={(upperHeight / Math.max(1, availableHeight)) * 100}
                    onresize={(event) => resize(event.clientY)}
                    onstep={resizeKey}
                    onreset={() => (splitRatio = null)}
                />
                {@render presentation.lower()}
            {/if}
        </main>
    </div>
    {#if inspectorOpen && presentation.inspector}<div class="workspace-inspector" data-workspace-background>
            {@render presentation.inspector()}
        </div>{/if}
    <footer class="status-bar">
        <span><span class="status-dot"></span>{status}</span><span class="ml-auto">{count}</span>
    </footer>
</div>

<!-- Keep fixed overlays outside the filtered header's containing block. -->
{#if preferencesOpen}
    {#await import('../../lib/components/PreferencesDialog.svelte') then module}
        <module.default
            {preferences}
            oncancel={() => (preferencesOpen = false)}
            onsaved={() => (preferencesOpen = false)}
        />
    {/await}
{/if}

<style>
    .modular-shell {
        --workspace-accent: #dc8b35;
    }
    .modular-shell[data-workspace-mode='files'] {
        --workspace-accent: #58bdb4;
    }
    .app-header {
        border-bottom: 2px solid var(--workspace-accent);
        column-gap: 8px;
    }
    .workspace-image-context {
        display: grid;
        gap: 2px;
        min-width: 0;
        padding: 0 8px;
    }
    .workspace-image-context strong {
        font-size: 11px;
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
    }
    .workspace-image-context small {
        font-size: 9px;
        color: var(--color-text-muted);
    }
    .workspace-mode-switch {
        grid-column: 4;
        display: flex;
        gap: 2px;
        border: 1px solid var(--color-border);
        border-radius: 4px;
        padding: 2px;
    }
    .workspace-mode-switch button {
        display: inline-flex;
        align-items: center;
        gap: 5px;
        height: 28px;
        padding: 0 8px;
        font-size: 11px;
        border: 0;
        border-radius: 3px;
        background: transparent;
        color: var(--color-text);
    }
    .workspace-mode-switch button.active {
        background: var(--workspace-accent);
        color: #101214;
    }
    .workspace-mode-switch button:disabled {
        opacity: 0.45;
    }
    .workspace-center {
        grid-row: 2;
        grid-column: 2;
        display: flex;
        flex-direction: column;
        min-width: 0;
        min-height: 0;
        overflow: hidden;
    }
    .workspace-view-toolbar {
        display: flex;
        align-items: center;
        min-height: 36px;
        border-bottom: 1px solid var(--color-border);
        gap: 8px;
    }
    .workspace-center > .main-stage {
        flex: 1;
    }
    .sidebar-closed .workspace-center {
        grid-column: 1;
    }
    .workspace-inspector {
        grid-row: 2;
        grid-column: -2;
        display: flex;
        flex-direction: column;
        min-width: 0;
        min-height: 0;
        overflow: hidden;
    }
    .workspace-inspector :global(> *) {
        flex: 1;
        min-height: 0;
    }
    .global-controls {
        grid-column: 3;
    }
    @media (max-width: 850px) {
        .workspace-inspector {
            display: none;
        }
        .workspace-image-context {
            display: none;
        }
        .app-header {
            grid-template-columns: var(--library-column-width) 1fr auto;
        }
        .global-controls {
            grid-column: 2;
            justify-content: flex-end;
        }
        .workspace-mode-switch {
            grid-column: 3;
        }
    }
</style>
