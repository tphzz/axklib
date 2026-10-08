<script lang="ts">
    import type { ProgramFormatMetadata } from '../../lib/objectEditing';
    import InspectorSection from '../../lib/components/InspectorSection.svelte';
    import AttributeHelp from '../../lib/components/AttributeHelp.svelte';
    import ProgramFormatBadge from './ProgramFormatBadge.svelte';
    let { format }: { format: ProgramFormatMetadata } = $props();
    const description = $derived(
        "The Program's stored representation: revisions 1 and 2 use A3000 storage; revision 4 uses A4000/A5000 storage. This does not identify the authoring sampler or guarantee that the complete volume will load. Linked objects and System Files retain their own formats." +
            (format.structurallyValid && format.headerRevision === 1
                ? ' Revision-1 effect values describe loading on A3000 V2 or later samplers, not original V1 playback.'
                : ''),
    );
</script>

<InspectorSection scope="program" sectionId="stored-format" title="Stored format" regionLabel="Program storage format">
    {#snippet badge()}<ProgramFormatBadge {format} />{/snippet}
    <dl class="metadata-list">
        <div>
            <dt>
                <AttributeHelp
                    label="Header revision"
                    contextKey={`${format.headerRevision}:${format.format}`}
                    {description}
                />
            </dt>
            <dd>{format.headerRevision}</dd>
        </div>
        <div>
            <dt>Assignments</dt>
            <dd>{format.storedAssignmentCount ?? 'Unavailable'}</dd>
        </div>
        <div>
            <dt>Stored row capacity</dt>
            <dd>{format.assignmentCapacity ?? 'Unavailable'}</dd>
        </div>
        <div>
            <dt>Parameter extension</dt>
            <dd>{format.parameterTailBytes === null ? 'Unavailable' : `${format.parameterTailBytes} bytes`}</dd>
        </div>
    </dl>
</InspectorSection>
