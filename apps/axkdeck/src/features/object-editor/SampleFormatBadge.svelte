<script lang="ts">
    import type { SampleFormatMetadata } from '../../lib/objectEditing';
    import StorageFormatBadge from './StorageFormatBadge.svelte';
    let { format }: { format: SampleFormatMetadata } = $props();
    const label = $derived(
        format.format === 'A3000_188' ? 'a3k' : format.format === 'A4000_A5000_224' ? 'a4k/a5k' : '?',
    );
    const warning = $derived(
        !format.structurallyValid || format.parameterIssues.length > 0 || format.diagnostics.length > 0,
    );
    const description = $derived(
        format.format === 'UNKNOWN'
            ? `Unknown Sample format. ${format.diagnostics.join(' ')}`
            : `${label}: stored ${format.parameterBytes}-byte parameter block. This identifies storage, not hardware-tested compatibility.${warning ? ' Stored parameter warnings are shown in the inspector.' : ''}${format.requiresA5000 ? ' Output routing uses A5000 effect slots.' : ''}`,
    );
</script>

<StorageFormatBadge {label} {description} {warning} />
