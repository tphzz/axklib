<script lang="ts">
    import type { ProgramFormatMetadata } from '../../lib/objectEditing';
    import StorageFormatBadge from './StorageFormatBadge.svelte';
    let { format }: { format: ProgramFormatMetadata } = $props();
    const label = $derived(
        !format.structurallyValid
            ? '?'
            : format.format === 'A3000'
              ? 'a3k'
              : format.format === 'A4000_A5000'
                ? 'a4k/a5k'
                : '?',
    );
    const description = $derived(
        label === '?'
            ? `Unrecognized Program storage, header revision ${format.headerRevision}. Format conversion is unavailable.`
            : `${label}: stored Program format, revision ${format.headerRevision}. Sample Banks, Samples and System Files retain their own formats. This is not a guarantee that the entire volume will load on that sampler.`,
    );
</script>

<StorageFormatBadge {label} {description} warning={!format.structurallyValid} />
