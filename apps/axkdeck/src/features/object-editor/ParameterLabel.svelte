<script lang="ts">
    import AttributeHelp from '../../lib/components/AttributeHelp.svelte';
    import { objectEditors } from './context';
    import type { EditorValue, EditorValues } from './draft.svelte';
    let {
        label,
        parameter,
        description = '',
        format,
        read,
        compact = false,
    }: {
        label: string;
        parameter: string;
        description?: string;
        format?: (value: EditorValue | undefined) => string;
        read?: (values: EditorValues) => EditorValue | undefined;
        compact?: boolean;
    } = $props();
    const editors = objectEditors();
    const different = $derived(editors?.comparison.differing(parameter, read) ?? false);
    const help = $derived(
        different
            ? `Different values\n${editors!.comparison.description(parameter, format, read)}${description ? `\n\n${description}` : ''}`
            : description,
    );
</script>

<span class:different class:compact data-parameter={parameter} data-different={different || undefined}>
    {#if !compact || different}
        {#if help}<AttributeHelp
                {label}
                description={help}
                contextKey={`${parameter}:${editors?.comparison.count ?? 0}`}
            >
                <span class="parameter-label-text">{label}</span>
                {#if different}<span class="difference-mark" aria-label="Different values">*</span>{/if}
            </AttributeHelp>{:else}<span class="parameter-label-text">{label}</span>{/if}
    {/if}
</span>

<style>
    .different {
        color: var(--color-warning, #e9a34d);
    }
    .difference-mark {
        margin-left: 3px;
        font-weight: 700;
    }
    .compact {
        display: inline-block;
        width: 12px;
        flex: 0 0 12px;
    }
    .compact .parameter-label-text {
        display: none;
    }
</style>
