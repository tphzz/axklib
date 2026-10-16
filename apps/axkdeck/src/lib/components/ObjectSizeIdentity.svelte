<script lang="ts">
    import { objectSizeSummary, objectSizeTooltip } from '../objectSizePresentation';
    import type { SamplerObject } from '../transport';
    import Icon from './Icon.svelte';
    import CollectionIdentity from './CollectionIdentity.svelte';
    import SampleFormatBadge from '../../features/object-editor/SampleFormatBadge.svelte';
    import ProgramFormatBadge from '../../features/object-editor/ProgramFormatBadge.svelte';
    import { objectEditors } from '../../features/object-editor/context';

    interface Props {
        name: string;
        object: SamplerObject;
        metadata?: string;
        indicator?: 'stereo';
    }

    let { name, object, metadata = '', indicator }: Props = $props();
    const tooltip = $derived(objectSizeTooltip(object));
    const editors = objectEditors();
    const dirty = $derived(
        editors?.documents.some((document) => document.detail?.object.id === object.key && document.draft.dirty) ??
            false,
    );
</script>

{#snippet indicators()}
    <span
        class="object-size-indicator object-size-dirty"
        class:dirty
        aria-hidden={!dirty}
        aria-label={dirty ? 'Unsaved Sample edits' : undefined}
        title={dirty ? 'Unsaved Sample edits' : undefined}>*</span
    >
    {#if indicator === 'stereo'}
        <span class="object-size-indicator" role="img" aria-label="Stereo Sample" title="Stereo Sample">
            <Icon name="stereo" size={12} />
        </span>
    {/if}
    {#if object.sampleFormat}<SampleFormatBadge format={object.sampleFormat} />{/if}
    {#if object.programFormat}<ProgramFormatBadge format={object.programFormat} />{/if}
{/snippet}
<CollectionIdentity
    {name}
    metadata={`${metadata ? `${metadata} · ` : ''}${objectSizeSummary(object)}`}
    nameTitle={`${name}\n${tooltip}`}
    metadataTitle={metadata ? `${metadata}\n${tooltip}` : tooltip}
    {indicators}
/>

<style>
    .object-size-indicator {
        display: inline-flex;
        flex: 0 0 auto;
        color: var(--color-accent);
    }
    .object-size-dirty {
        width: 8px;
        height: 10px;
        align-items: center;
        justify-content: center;
        font-size: 10px;
        line-height: 10px;
        visibility: hidden;
    }
    .object-size-dirty.dirty {
        visibility: visible;
    }
</style>
