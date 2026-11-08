<script lang="ts">
    import type { KnownSampleFormat } from '../aSeriesPreferences.svelte';
    let {
        value = $bindable(),
        label,
        disabled = false,
        compact = false,
    }: {
        value: KnownSampleFormat;
        label: string;
        disabled?: boolean;
        compact?: boolean;
    } = $props();
</script>

<div class="dialog-segmented-control" class:dialog-segmented-control--compact={compact} role="group" aria-label={label}>
    <button type="button" aria-pressed={value === 'A3000_188'} {disabled} onclick={() => (value = 'A3000_188')}
        >{compact ? 'A3K' : 'a3k'}</button
    >
    <button
        type="button"
        aria-pressed={value === 'A4000_A5000_224'}
        {disabled}
        onclick={() => (value = 'A4000_A5000_224')}>{compact ? 'A4K/A5K' : 'a4k/a5k'}</button
    >
</div>
