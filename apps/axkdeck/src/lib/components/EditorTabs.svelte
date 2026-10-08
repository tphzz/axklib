<script lang="ts" generics="T extends string">
    import { tick } from 'svelte';

    let {
        tabs,
        active,
        onselect,
        label,
        idPrefix,
        panelId,
        secondary = false,
    }: {
        tabs: readonly { id: T; label: string }[];
        active: T;
        onselect: (id: T) => void;
        label: string;
        idPrefix?: string;
        panelId?: string;
        secondary?: boolean;
    } = $props();
    let strip: HTMLElement;

    function reveal(button: HTMLElement | null) {
        if (!button || !strip) return;
        // Only scroll this strip, not the workspace or the editor's content pane.
        const viewport = strip.getBoundingClientRect();
        if (!viewport.width || !strip.clientWidth) return;
        const bounds = button.getBoundingClientRect();
        const scale = viewport.width / strip.clientWidth;
        if (bounds.left < viewport.left)
            strip.scrollLeft = Math.max(0, strip.scrollLeft + Math.floor((bounds.left - viewport.left) / scale));
        else if (bounds.right > viewport.right) strip.scrollLeft += Math.ceil((bounds.right - viewport.right) / scale);
    }
    function revealSelected() {
        reveal(strip?.querySelector('[aria-selected="true"], [aria-pressed="true"]'));
    }
    function watchWidth(node: HTMLElement) {
        const observer = new ResizeObserver(revealSelected);
        observer.observe(node);
        return { destroy: () => observer.disconnect() };
    }
    $effect(() => {
        void active;
        void tick().then(revealSelected);
    });
    function move(event: KeyboardEvent, index: number) {
        if (event.altKey || event.ctrlKey || event.metaKey) return;
        let next = index;
        if (event.key === 'ArrowRight') next = (index + 1) % tabs.length;
        else if (event.key === 'ArrowLeft') next = (index + tabs.length - 1) % tabs.length;
        else if (event.key === 'Home') next = 0;
        else if (event.key === 'End') next = tabs.length - 1;
        else return;
        event.preventDefault();
        onselect(tabs[next]!.id);
        const button = strip.children[next] as HTMLButtonElement;
        button.focus({ preventScroll: true });
        reveal(button);
    }
</script>

<nav
    class="editor-tabs"
    class:secondary
    role={secondary ? undefined : 'tablist'}
    aria-label={label}
    bind:this={strip}
    use:watchWidth
>
    {#each tabs as tab, index (tab.id)}
        <button
            type="button"
            role={secondary ? undefined : 'tab'}
            id={idPrefix ? `${idPrefix}-${tab.id}` : undefined}
            aria-controls={panelId}
            aria-selected={secondary ? undefined : active === tab.id}
            aria-pressed={secondary ? active === tab.id : undefined}
            tabindex={secondary || active === tab.id ? 0 : -1}
            onclick={() => onselect(tab.id)}
            onfocus={(event) => reveal(event.currentTarget)}
            onkeydown={(event) => move(event, index)}>{tab.label}</button
        >
    {/each}
</nav>

<style>
    .editor-tabs {
        position: relative;
        display: flex;
        align-self: stretch;
        min-width: 0;
        overflow-x: auto;
        scrollbar-width: none;
    }
    .editor-tabs::-webkit-scrollbar {
        display: none;
    }
    button {
        flex: 0 0 auto;
        padding: 0 8px;
        border: 0;
        border-bottom: 2px solid transparent;
        background: transparent;
        color: var(--color-text-muted);
        font-size: 11px;
        white-space: nowrap;
    }
    button:hover,
    button[aria-selected='true'],
    button[aria-pressed='true'] {
        color: var(--color-text);
        background: var(--color-panel-raised);
    }
    button[aria-selected='true'] {
        border-bottom-color: var(--color-accent);
    }
    button:focus-visible {
        outline: 1px solid var(--color-accent);
        outline-offset: -2px;
    }
    .secondary {
        align-items: center;
        gap: 3px;
    }
    .secondary button {
        height: 24px;
        border: 1px solid transparent;
        border-radius: 3px;
    }
    .secondary button[aria-pressed='true'] {
        border-color: var(--color-accent);
    }
</style>
