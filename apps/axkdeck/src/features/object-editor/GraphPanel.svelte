<script lang="ts">
    import { getContext, type Snippet } from 'svelte';
    import { editorScrollContext, rememberEditorScroll, type EditorScroll } from './editorScroll';
    import { measureWidth } from './measureWidth';
    import { graphLayout, graphSplitRatio } from './graphLayout.svelte';
    import { initialEditorWidth } from './editorWidth';
    import Splitter from '../../lib/components/Splitter.svelte';
    let {
        graph,
        controls,
        label,
        layoutKey = '',
        graphMinimum = 360,
        controlsMinimum = 360,
        stackBelow = 900,
        graphMinHeight = 152,
    }: {
        graph: Snippet;
        controls: Snippet;
        label: string;
        layoutKey?: string;
        graphMinimum?: number;
        controlsMinimum?: number;
        stackBelow?: number;
        graphMinHeight?: number;
    } = $props();
    let host: HTMLDivElement;
    const scroll = getContext<(() => EditorScroll) | undefined>(editorScrollContext);
    let width = $state(initialEditorWidth());
    const clamp = (value: number) => graphSplitRatio(value, width - 8, graphMinimum, controlsMinimum);
    const ratio = $derived(clamp(layoutKey ? (graphLayout.views[layoutKey] ?? 0.5) : graphLayout.ratio));
    function setRatio(value: number) {
        if (layoutKey) graphLayout.views[layoutKey] = clamp(value);
        else graphLayout.ratio = clamp(value);
    }
    function resize(event: PointerEvent) {
        const rect = host.getBoundingClientRect();
        setRatio((((event.clientX - rect.left) / rect.width) * width - 4) / Math.max(1, width - 8));
    }
</script>

<div
    class="graph-panel"
    bind:this={host}
    use:measureWidth={{ scope: 'graph', change: (value) => (width = value) }}
    class:stacked={width < stackBelow}
    class:custom={!!layoutKey}
    style:--graph-ratio={ratio}
    style:--graph-min-height={`${graphMinHeight}px`}
>
    <div class="graph-region" role="group" aria-label={`${label} graph`}>{@render graph()}</div>
    {#if width >= stackBelow}<Splitter
            orientation="vertical"
            label="Resize graph and controls"
            value={ratio * 100}
            min={clamp(0) * 100}
            max={clamp(1) * 100}
            onresize={resize}
            onreset={() => setRatio(0.5)}
            onstep={(key) => {
                setRatio(key === 'Home' ? 0 : key === 'End' ? 1 : ratio + (key === 'ArrowLeft' ? -0.03 : 0.03));
            }}
        />{/if}
    <div
        class="graph-controls"
        use:measureWidth={{ scope: 'controls' }}
        use:rememberEditorScroll={scroll ? { ...scroll(), key: `${scroll().key}:controls` } : undefined}
        role="group"
        aria-label={`${label} controls`}
    >
        {@render controls()}
    </div>
</div>

<style>
    .graph-panel {
        container: graph-panel / inline-size;
        display: grid;
        grid-template-columns: minmax(0, calc((100% - 8px) * var(--graph-ratio))) 8px minmax(0, 1fr);
        height: 100%;
        min-height: 0;
        min-width: 0;
    }
    .graph-region {
        display: flex;
        flex-direction: column;
        min-width: 0;
        min-height: 0;
        gap: 6px;
        overflow: auto;
    }
    .graph-controls {
        min-width: 0;
        min-height: 0;
        overflow: auto;
        padding: 0 8px;
        scrollbar-gutter: stable;
        container: graph-controls / inline-size;
    }
    .stacked {
        grid-template-columns: minmax(0, 1fr);
        grid-template-rows: minmax(var(--graph-min-height), 1fr) minmax(72px, 1fr);
        gap: 8px;
    }
    .stacked .graph-controls {
        border-top: 1px solid var(--color-border);
        padding: 6px 0 0;
    }
    .stacked.custom {
        overflow: auto;
    }
</style>
