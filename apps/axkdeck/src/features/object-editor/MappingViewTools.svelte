<script lang="ts">
    import EditorNumber from './EditorNumber.svelte';
    import Icon from '../../lib/components/Icon.svelte';
    let {
        start,
        span,
        minimum,
        busy,
        audition,
        velocity,
        onvelocity,
        onpan,
        onzoom,
        onfit,
    }: {
        start: number;
        span: number;
        minimum: number;
        busy: boolean;
        audition: boolean;
        velocity: number;
        onvelocity: (velocity: number) => void;
        onpan: (delta: number) => void;
        onzoom: (factor: number) => void;
        onfit: () => void;
    } = $props();
</script>

<div class="view-tools">
    {#if audition}<div class="audition-velocity">
            <span>Velocity</span>
            <EditorNumber label="Audition velocity" value={velocity} min={1} max={127} onchange={onvelocity} />
        </div>{/if}
    <div class="viewport-tools">
        <button
            class="editor-icon pan-left"
            aria-label="Pan keyboard left"
            title="Pan left"
            disabled={busy || start === 0}
            onclick={() => onpan(-12)}><Icon name="chevron" size={14} /></button
        >
        <button
            class="editor-icon"
            aria-label="Pan keyboard right"
            title="Pan right"
            disabled={busy || start + span >= 128}
            onclick={() => onpan(12)}><Icon name="chevron" size={14} /></button
        >
        <button
            class="editor-icon"
            aria-label="Zoom keyboard out"
            title="Zoom out"
            disabled={busy || span === 128}
            onclick={() => onzoom(2)}><Icon name="zoom-out" size={14} /></button
        >
        <button
            class="editor-icon"
            aria-label="Zoom keyboard in"
            title="Zoom in"
            disabled={busy || span <= minimum}
            onclick={() => onzoom(0.5)}><Icon name="zoom-in" size={14} /></button
        >
        <button class="editor-icon" aria-label="Fit keyboard ranges" title="Fit ranges" disabled={busy} onclick={onfit}
            ><Icon name="fit-width" size={14} /></button
        >
    </div>
</div>

<style>
    .view-tools {
        display: flex;
        flex-wrap: wrap;
        justify-content: flex-end;
        gap: 4px;
        margin-left: auto;
        max-width: 100%;
        flex: none;
    }
    .viewport-tools {
        display: flex;
        gap: 4px;
        flex: none;
    }
    .audition-velocity {
        display: flex;
        align-items: center;
        gap: 6px;
        width: 190px;
        max-width: 100%;
        font-size: 10px;
    }
    .pan-left :global(svg) {
        transform: rotate(180deg);
    }
</style>
