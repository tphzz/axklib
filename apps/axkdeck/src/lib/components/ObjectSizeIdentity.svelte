<script lang="ts">
    import { objectSizeSummary, objectSizeTooltip } from '../objectSizePresentation';
    import type { SamplerObject } from '../transport';
    import Icon from './Icon.svelte';
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

<span class="object-size-primary">
    <strong title={tooltip}>{name}</strong>
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
</span>
<small class="object-size-secondary" title={tooltip}>
    {metadata ? `${metadata} · ` : ''}{objectSizeSummary(object)}
</small>

<style>
    .object-size-primary {
        display: flex;
        min-width: 0;
        align-items: center;
        gap: 3px;
    }

    .object-size-primary strong {
        display: block;
        min-width: 0;
        overflow: hidden;
        color: var(--color-text-strong);
        font-size: 10px;
        font-weight: 600;
        line-height: 10px;
        text-overflow: ellipsis;
        white-space: nowrap;
    }

    .object-size-secondary {
        display: block;
        margin-top: 0;
        overflow: hidden;
        color: var(--color-text-muted);
        font-size: 8.5px;
        line-height: 9px;
        text-overflow: ellipsis;
        white-space: nowrap;
    }

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
