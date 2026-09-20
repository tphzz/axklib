<script lang="ts">
    import { setContext, type Snippet } from 'svelte';
    import { inspectorSectionVisibility, provideInspectorPanels } from '../inspectorPanels.svelte';
    import Icon from './Icon.svelte';

    let {
        scope,
        sectionId,
        title,
        badge,
        children,
        class: className = '',
        regionLabel = title,
    }: {
        scope: string;
        sectionId: string;
        title: string;
        badge?: Snippet;
        children: Snippet;
        class?: string;
        regionLabel?: string;
    } = $props();
    const panels = provideInspectorPanels();
    const id = $props.id();
    const expanded = $derived(panels.expanded(scope, sectionId));
    let trigger: HTMLButtonElement;
    let body: HTMLDivElement;
    setContext(inspectorSectionVisibility, () => expanded);

    function toggle() {
        if (expanded && body?.contains(document.activeElement)) trigger.focus();
        panels.setExpanded(scope, sectionId, !expanded);
    }
</script>

<section class={`inspector-section ${className}`} data-inspector-section={sectionId} aria-label={regionLabel}>
    <h4 class="inspector-section-heading">
        <button
            bind:this={trigger}
            type="button"
            aria-label={title}
            aria-describedby={badge ? `${id}-badge` : undefined}
            aria-expanded={expanded}
            aria-controls={id}
            onclick={toggle}
        >
            <span class="inspector-section-title"
                >{title}{#if badge}{' '}<span id={`${id}-badge`} class="inspector-section-badge">{@render badge()}</span
                    >{/if}</span
            >
            <span class="inspector-section-chevron" class:expanded><Icon name="chevron" size={12} /></span>
        </button>
    </h4>
    <div bind:this={body} {id} class="inspector-section-body" hidden={!expanded}>
        {@render children()}
    </div>
</section>

<style>
    .inspector-section-heading {
        margin: 0;
    }
    .inspector-section-heading button {
        display: flex;
        align-items: center;
        justify-content: space-between;
        width: 100%;
        min-height: 24px;
        gap: 6px;
        padding: 0;
        border: 0;
        background: transparent;
        color: inherit;
        font: inherit;
        text-align: left;
        cursor: pointer;
    }
    .inspector-section-heading button:hover {
        color: var(--color-accent);
    }
    .inspector-section-heading button:focus-visible {
        outline: 1px solid var(--color-accent);
        outline-offset: 2px;
    }
    .inspector-section-title {
        display: flex;
        align-items: center;
        gap: 6px;
        min-width: 0;
        overflow-wrap: anywhere;
    }
    .inspector-section-chevron {
        display: flex;
        flex: 0 0 auto;
        transform: none;
    }
    .inspector-section-badge {
        display: inline-flex;
    }
    .inspector-section-chevron.expanded {
        transform: rotate(90deg);
    }
    .inspector-section-body[hidden] {
        display: none;
    }
</style>
