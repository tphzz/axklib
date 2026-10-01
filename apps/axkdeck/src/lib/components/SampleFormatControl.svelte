<script lang="ts">
    import type { KnownSampleFormat } from '../aSeriesPreferences.svelte';
    let {
        value = $bindable(),
        label,
        disabled = false,
    }: {
        value: KnownSampleFormat;
        label: string;
        disabled?: boolean;
    } = $props();
</script>

<div class="dialog-segmented-control" role="group" aria-label={label}>
    <button type="button" aria-pressed={value === 'A3000_188'} {disabled} onclick={() => (value = 'A3000_188')}
        >a3k</button
    >
    <button
        type="button"
        aria-pressed={value === 'A4000_A5000_224'}
        {disabled}
        onclick={() => (value = 'A4000_A5000_224')}>a4k/a5k</button
    >
</div>
