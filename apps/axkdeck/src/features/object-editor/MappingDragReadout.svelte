<script lang="ts">
    import { mappingDragPosition, mappingDragText, type MappingFeedback } from './mappingFeedback';
    let {
        feedback,
        formatNote,
        start,
        span,
        width,
        height,
    }: {
        feedback: MappingFeedback;
        formatNote: (note: number) => string;
        start: number;
        span: number;
        width: number;
        height: number;
    } = $props();
    let output = $state<HTMLOutputElement>();
    let labelWidth = $state(0),
        labelHeight = $state(22);
    const text = $derived(mappingDragText(feedback, formatNote));
    const position = $derived(mappingDragPosition(feedback, start, span, width, height, labelWidth, labelHeight));
    $effect(() => {
        if (!output || typeof ResizeObserver === 'undefined') return;
        const observer = new ResizeObserver(([entry]) => {
            labelWidth = entry.borderBoxSize[0]?.inlineSize ?? entry.contentRect.width;
            labelHeight = entry.borderBoxSize[0]?.blockSize ?? entry.contentRect.height;
        });
        observer.observe(output);
        return () => observer.disconnect();
    });
</script>

<output
    bind:this={output}
    data-drag-readout
    aria-live="off"
    style:left={`${position.left}px`}
    style:top={`${position.top}px`}>{text}</output
>

<style>
    output {
        position: absolute;
        z-index: 1;
        pointer-events: none;
        max-width: calc(100% - 8px);
        overflow: hidden;
        white-space: nowrap;
        text-overflow: ellipsis;
        padding: 3px 5px;
        font: 10px var(--font-mono, monospace);
        line-height: 14px;
        color: var(--color-text);
        background: var(--color-panel-deep);
        border: 1px solid var(--color-accent);
        border-radius: 2px;
    }
</style>
