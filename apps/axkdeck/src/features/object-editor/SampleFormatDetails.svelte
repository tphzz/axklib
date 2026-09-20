<script lang="ts">
    import type { SampleFormatMetadata } from '../../lib/objectEditing';
    import SampleFormatBadge from './SampleFormatBadge.svelte';
    import InspectorSection from '../../lib/components/InspectorSection.svelte';
    let { format }: { format: SampleFormatMetadata } = $props();
</script>

<InspectorSection scope="sample" sectionId="stored-format" title="Stored format" regionLabel="Sample storage format">
    {#snippet badge()}<SampleFormatBadge {format} />{/snippet}
    <dl class="metadata-list">
        <div>
            <dt>Parameter block</dt>
            <dd>{format.parameterBytes === null ? 'Unknown' : `${format.parameterBytes} bytes`}</dd>
        </div>
        <div>
            <dt>Header revision</dt>
            <dd>{format.headerRevision}</dd>
        </div>
        {#if format.format === 'A4000_A5000_224'}
            <div>
                <dt>Extension</dt>
                <dd>36 bytes</dd>
            </div>
            <div>
                <dt>Extension settings</dt>
                <dd>
                    {format.extensionDiffersFromPrefixDefaults
                        ? 'Differ from prefix defaults'
                        : 'Match prefix defaults'}
                </dd>
            </div>
        {/if}
        {#if format.requiresA5000}<div>
                <dt>Output requirement</dt>
                <dd>A5000 effects 4-6</dd>
            </div>{/if}
    </dl>
    {#each format.diagnostics as diagnostic}<p>{diagnostic}</p>{/each}
    {#each format.parameterIssues as issue}<p class="format-issue">
            {issue.key}{issue.storedValue === null ? '' : ` (${issue.storedValue})`}: {issue.message}
        </p>{/each}
    <p>Format identification is not hardware certification. Test authored media on the intended sampler.</p>
</InspectorSection>

<style>
    p {
        font-size: 10px;
        color: var(--color-text-muted);
        overflow-wrap: anywhere;
    }
    .format-issue {
        color: var(--color-warning, #e6a34c);
    }
</style>
