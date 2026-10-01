<script lang="ts">
    import ExtendedParameterMarker from './ExtendedParameterMarker.svelte';
    let { label, extended = false }: { label: string; extended?: boolean } = $props();
</script>

<span class="editor-option-label">
    <span class="editor-option-text">{label}</span>
    {#if extended}<ExtendedParameterMarker decorative />{/if}
</span>
