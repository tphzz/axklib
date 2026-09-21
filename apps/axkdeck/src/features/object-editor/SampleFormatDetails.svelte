<script lang="ts">
    import type { SampleFormatMetadata } from '../../lib/objectEditing';
    import SampleFormatBadge from './SampleFormatBadge.svelte';
    import InspectorSection from '../../lib/components/InspectorSection.svelte';
    import type { SampleStructureItem } from '../../lib/types';
    let {
        format,
        bank = false,
        members = [],
        unresolved = 0,
    }: {
        format: SampleFormatMetadata;
        bank?: boolean;
        members?: SampleStructureItem[];
        unresolved?: number;
    } = $props();
    const formats = $derived.by(() => {
        const unique = new Map(members.map((member) => [member.objectId, member.object.sampleFormat]));
        const counts = { A3000_188: 0, A4000_A5000_224: 0, UNKNOWN: 0 };
        for (const item of unique.values()) counts[item?.structurallyValid ? item.format : 'UNKNOWN']++;
        return counts;
    });
</script>

<InspectorSection
    scope={bank ? 'sample-bank' : 'sample'}
    sectionId="stored-format"
    title="Stored format"
    regionLabel={bank ? 'Sample Bank storage format' : 'Sample storage format'}
>
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
        {#if bank}
            <div>
                <dt>Member Sample formats</dt>
                <dd>{formats.A3000_188} a3k, {formats.A4000_A5000_224} a4k/a5k</dd>
            </div>
            {#if formats.UNKNOWN}<div>
                    <dt>Unknown member formats</dt>
                    <dd>{formats.UNKNOWN}</dd>
                </div>{/if}
            {#if unresolved}<div>
                    <dt>Unresolved member references</dt>
                    <dd>{unresolved}</dd>
                </div>{/if}
        {/if}
    </dl>
    {#each format.diagnostics as diagnostic}<p>{diagnostic}</p>{/each}
    {#each format.parameterIssues as issue}<p class="format-issue">
            {issue.key}{issue.storedValue === null ? '' : ` (${issue.storedValue})`}: {issue.message}
        </p>{/each}
    <p>Format identification is not hardware certification. Test authored media on the intended sampler.</p>
    {#if bank}<p>The badge describes the bank itself. Member Samples retain their own formats.</p>{/if}
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
