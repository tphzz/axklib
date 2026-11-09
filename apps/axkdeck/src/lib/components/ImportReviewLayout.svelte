<script lang="ts">
    import { tick, type Snippet } from 'svelte';
    import TabStrip from './TabStrip.svelte';
    let {
        contents,
        review,
        columns,
        contentsFooter,
        reviewFooter,
        reviewToolbar,
        contentsSummary = '',
        reviewSummary = '',
        reviewNotice = '',
        active = $bindable<'contents' | 'review'>('contents'),
    }: {
        contents: Snippet;
        review: Snippet;
        columns?: Snippet;
        contentsFooter?: Snippet;
        reviewFooter?: Snippet;
        reviewToolbar?: Snippet;
        contentsSummary?: string;
        reviewSummary?: string;
        reviewNotice?: string;
        active?: 'contents' | 'review';
    } = $props();
    const id = $props.id();
    let width = $state(0);
    let root: HTMLDivElement;
    const narrow = $derived(width > 0 && width < 736);
    $effect.pre(() => {
        if (!narrow) return;
        const current = active;
        // Resizing must not leave keyboard focus inside the now-hidden pane.
        const focused = document.activeElement;
        if (focused && root?.querySelector(`[data-pane]:not([data-pane="${current}"])`)?.contains(focused)) {
            void tick().then(() => root?.querySelector<HTMLButtonElement>(`[data-tab="${current}"]`)?.focus());
        }
    });
</script>

<div class="import-review-layout" class:narrow bind:clientWidth={width} bind:this={root}>
    {#if narrow}
        <TabStrip
            fill
            tabs={[
                { id: 'contents', label: 'Contents', panelId: `${id}-contents-panel` },
                {
                    id: 'review',
                    label: reviewNotice ? `Review · ${reviewNotice}` : 'Review',
                    panelId: `${id}-review-panel`,
                },
            ]}
            {active}
            onselect={(pane) => {
                active = pane;
            }}
            label="Import views"
            idPrefix={id}
        />
    {/if}
    <div class="panes">
        {#each ['contents', 'review'] as pane}
            {@const inactive = narrow && active !== pane}
            <section
                class="import-pane"
                class:inactive
                data-pane={pane}
                id={`${id}-${pane}-panel`}
                role={narrow ? 'tabpanel' : 'region'}
                aria-label={narrow ? undefined : pane === 'contents' ? 'Contents' : 'Review'}
                aria-labelledby={narrow ? `${id}-${pane}` : undefined}
                aria-hidden={inactive}
                inert={inactive}
            >
                <header class="pane-heading">
                    <h3>{pane === 'contents' ? 'Contents' : 'Review'}</h3>
                    <span>{pane === 'contents' ? contentsSummary : reviewSummary}</span>
                </header>
                {#if pane === 'contents' && columns}<div class="pane-columns">{@render columns()}</div>{/if}
                {#if pane === 'review' && reviewToolbar}<div class="pane-toolbar">{@render reviewToolbar()}</div>{/if}
                <!-- svelte-ignore a11y_no_noninteractive_tabindex (Both bounded panes support keyboard scrolling.) -->
                <div
                    class="import-pane-scroll"
                    tabindex="0"
                    role="region"
                    aria-label={pane === 'contents' ? 'Import contents' : 'Import review'}
                >
                    {#if pane === 'contents'}{@render contents()}{:else}{@render review()}{/if}
                </div>
                {#if pane === 'contents' && contentsFooter}<div class="pane-footer">
                        {@render contentsFooter()}
                    </div>{/if}
                {#if pane === 'review' && reviewFooter}<div class="pane-footer">{@render reviewFooter()}</div>{/if}
            </section>
        {/each}
    </div>
</div>

<style>
    .import-review-layout {
        display: flex;
        flex: 1;
        flex-direction: column;
        min-width: 0;
        min-height: 0;
    }
    .panes {
        display: grid;
        grid-template-columns: minmax(0, 56fr) minmax(0, 44fr);
        gap: 12px;
        flex: 1;
        min-height: 0;
    }
    .import-pane {
        display: flex;
        flex-direction: column;
        min-width: 0;
        min-height: 0;
    }
    .pane-heading {
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 8px;
        min-height: 28px;
        flex: none;
    }
    h3 {
        margin: 0;
        font-size: var(--dialog-section-font-size);
        font-weight: 600;
        color: var(--color-text-strong);
    }
    .pane-heading span {
        color: var(--color-text-muted);
        font-size: var(--dialog-metadata-font-size);
        text-align: right;
        overflow-wrap: anywhere;
    }
    .pane-columns {
        flex: none;
        min-height: 24px;
        background: var(--color-panel-raised);
        color: var(--color-text);
    }
    .pane-columns,
    .import-pane-scroll {
        padding-right: var(--overlay-scrollbar-clearance);
        overflow-y: auto;
        scrollbar-gutter: stable;
    }
    .import-pane-scroll {
        flex: 1;
        min-height: 0;
        overflow-wrap: anywhere;
    }
    .import-pane-scroll:focus-visible {
        outline: 1px solid var(--state-focus);
        outline-offset: -1px;
    }
    .pane-footer,
    .pane-toolbar {
        flex: none;
        min-height: 28px;
    }
    .narrow .panes {
        grid-template-columns: minmax(0, 1fr);
        gap: 0;
    }
    .narrow .import-pane {
        grid-area: 1 / 1;
    }
    /* Keep both scroll boxes laid out so switching views does not reset positions. */
    .inactive {
        visibility: hidden;
        pointer-events: none;
    }
</style>
