<script lang="ts">
    import ExtendedParameterMarker from './ExtendedParameterMarker.svelte';
    let {
        label,
        extended = false,
        a5000Only = false,
    }: { label: string; extended?: boolean; a5000Only?: boolean } = $props();
</script>

<span class="editor-option-label">
    <span class="editor-option-text">{label}</span>
    {#if extended || a5000Only}<ExtendedParameterMarker decorative {a5000Only} />{/if}
</span>
