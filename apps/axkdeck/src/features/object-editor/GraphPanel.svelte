<script lang="ts">
    import { getContext, type Snippet } from 'svelte';
    import { editorScrollContext, rememberEditorScroll, type EditorScroll } from './editorScroll';
    import { measureWidth } from './measureWidth';
    import { graphLayout, graphSplitRatio } from './graphLayout.svelte';
    import Splitter from '../../lib/components/Splitter.svelte';
    let { graph, controls, label }: { graph: Snippet; controls: Snippet; label: string } = $props();
    let host: HTMLDivElement;
    const scroll = getContext<(() => EditorScroll) | undefined>(editorScrollContext);
    let width = $state(0);
    const ratio = $derived(graphSplitRatio(graphLayout.ratio, width - 8));
    function resize(event: PointerEvent) {
        const rect = host.getBoundingClientRect();
        graphLayout.ratio = graphSplitRatio(
            (((event.clientX - rect.left) / rect.width) * width - 4) / Math.max(1, width - 8),
            width - 8,
        );
    }
</script>

<div
    class="graph-panel"
    bind:this={host}
    use:measureWidth={{ scope: 'graph', change: (value) => (width = value) }}
    class:stacked={width < 900}
    style:--graph-ratio={ratio}
>
    <div class="graph-region" role="group" aria-label={`${label} graph`}>{@render graph()}</div>
    {#if width >= 900}<Splitter
            orientation="vertical"
            label="Resize graph and controls"
            value={ratio * 100}
            min={graphSplitRatio(0, width - 8) * 100}
            max={graphSplitRatio(1, width - 8) * 100}
            onresize={resize}
            onreset={() => (graphLayout.ratio = 0.5)}
            onstep={(key) => {
                graphLayout.ratio = graphSplitRatio(
                    key === 'Home' ? 0 : key === 'End' ? 1 : ratio + (key === 'ArrowLeft' ? -0.03 : 0.03),
                    width - 8,
                );
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
        grid-template-rows: minmax(152px, 1fr) minmax(72px, 1fr);
        gap: 8px;
    }
    .stacked .graph-controls {
        border-top: 1px solid var(--color-border);
        padding: 6px 0 0;
    }
</style>
