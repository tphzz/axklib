<script lang="ts">
    import type { SampleFormatMetadata } from '../../lib/objectEditing';
    let { format }: { format: SampleFormatMetadata } = $props();
    const label = $derived(
        format.format === 'A3000_188' ? 'a3k' : format.format === 'A4000_A5000_224' ? 'a4k/a5k' : '?',
    );
    const warning = $derived(!format.structurallyValid || format.parameterIssues.length > 0);
    const description = $derived(
        format.format === 'UNKNOWN'
            ? `Unknown Sample format. ${format.diagnostics.join(' ')}`
            : `${label}: stored ${format.parameterBytes}-byte parameter block. This identifies storage, not hardware-tested compatibility.${warning ? ' Stored parameter warnings are shown in the inspector.' : ''}${format.requiresA5000 ? ' Output routing uses A5000 effect slots.' : ''}`,
    );
</script>

<span class="format-badge" class:warning title={description} aria-label={description}>{label}</span>

<style>
    .format-badge {
        display: inline-flex;
        flex: 0 0 auto;
        align-items: center;
        justify-content: center;
        height: 13px;
        padding: 0 3px;
        border: 1px solid currentColor;
        border-radius: 3px;
        color: var(--color-text-muted);
        font-size: 8px;
        line-height: 11px;
        white-space: nowrap;
    }
    .warning {
        color: var(--color-warning, #e6a34c);
    }
</style>
