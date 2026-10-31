<script lang="ts" generics="T extends string">
    import { onDestroy, tick, type ComponentProps } from 'svelte';
    import Icon from './Icon.svelte';

    let {
        tabs,
        active,
        onselect,
        label,
        appearance = 'content',
        semantics = 'tablist',
        disabled = false,
        idPrefix,
        panelId,
    }: {
        tabs: readonly {
            id: T;
            label: string;
            icon?: ComponentProps<typeof Icon>['name'];
            disabled?: boolean;
            reason?: string;
            describedBy?: string;
            buttonId?: string;
            panelId?: string;
        }[];
        active?: T;
        onselect: (id: T) => void | Promise<void>;
        label: string;
        appearance?: 'workspace' | 'content';
        semantics?: 'tablist' | 'navigation';
        disabled?: boolean;
        idPrefix?: string;
        panelId?: string;
    } = $props();
    let strip: HTMLElement;
    let request = 0;
    let alive = true;
    const focusWatches = new Set<() => void>();
    let keyboardOrigin: Element | null = null;
    const tabStop = $derived(
        tabs.find((tab) => !tab.disabled && tab.id === active)?.id ?? tabs.find((tab) => !tab.disabled)?.id,
    );
    onDestroy(() => {
        alive = false;
        request++;
        for (const cleanup of focusWatches) cleanup();
    });

    function reveal(button: HTMLElement | null) {
        if (!button || !strip || !alive) return;
        // Scroll only this strip; leave workspace and editor content positions intact.
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
        void tabs;
        void tick().then(revealSelected);
    });
    async function select(id: T, button: HTMLButtonElement, keyboard = false) {
        if (button.matches(':disabled')) return;
        const version = ++request;
        const doc = strip.ownerDocument;
        const origin = doc.activeElement;
        if (keyboard) keyboardOrigin = origin;
        let departed = false;
        const focus = (event: FocusEvent) => {
            if (event.target !== origin && event.target !== doc && event.target !== doc.body) departed = true;
        };
        const pointer = () => {
            departed = true;
        };
        const key = (event: KeyboardEvent) => {
            if (event.key === 'Tab') departed = true;
        };
        const cleanup = () => {
            doc.removeEventListener('focusin', focus);
            doc.removeEventListener('pointerdown', pointer);
            doc.removeEventListener('keydown', key);
            doc.defaultView?.removeEventListener('blur', pointer);
            focusWatches.delete(cleanup);
        };
        if (keyboard) {
            doc.addEventListener('focusin', focus);
            doc.addEventListener('pointerdown', pointer);
            doc.addEventListener('keydown', key);
            doc.defaultView?.addEventListener('blur', pointer);
            focusWatches.add(cleanup);
        }
        try {
            await onselect(id);
            await tick();
        } finally {
            cleanup();
            if (version === request) keyboardOrigin = null;
        }
        if (!alive || version !== request || active !== id || !button.isConnected || button.matches(':disabled'))
            return;
        // Busy source transitions can natively blur a disabled origin to BODY.
        if (
            keyboard &&
            !departed &&
            strip.contains(origin) &&
            (doc.activeElement === origin || doc.activeElement === doc.body)
        ) {
            button.focus({ preventScroll: true });
            reveal(button);
        }
    }
    function move(event: KeyboardEvent) {
        if (event.altKey || event.ctrlKey || event.metaKey) return;
        const current = event.currentTarget as HTMLButtonElement;
        if (current.matches(':disabled')) return;
        const enabled = [...strip.querySelectorAll<HTMLButtonElement>('button')].filter(
            (button) => !button.matches(':disabled'),
        );
        const base =
            keyboardOrigin === current ? (enabled.find((button) => button.dataset.tab === active) ?? current) : current;
        const index = enabled.indexOf(base);
        let next: number;
        if (event.key === 'ArrowRight') next = (index + 1) % enabled.length;
        else if (event.key === 'ArrowLeft') next = (index + enabled.length - 1) % enabled.length;
        else if (event.key === 'Home') next = 0;
        else if (event.key === 'End') next = enabled.length - 1;
        else return;
        event.preventDefault();
        const button = enabled[next];
        const tab = tabs.find((tab) => tab.id === button?.dataset.tab);
        if (tab && button) void select(tab.id, button, true);
    }
</script>

<nav
    class="tab-strip"
    data-appearance={appearance}
    role={semantics === 'tablist' ? 'tablist' : undefined}
    aria-label={label}
    bind:this={strip}
    use:watchWidth
>
    {#each tabs as tab (tab.id)}
        <button
            type="button"
            role={semantics === 'tablist' ? 'tab' : undefined}
            id={tab.buttonId ?? (idPrefix ? `${idPrefix}-${tab.id}` : undefined)}
            data-tab={tab.id}
            class:active={active === tab.id}
            aria-label={tab.label}
            title={tab.reason || tab.label}
            aria-describedby={tab.describedBy}
            aria-controls={tab.panelId ?? panelId}
            aria-selected={semantics === 'tablist' ? active === tab.id : undefined}
            aria-pressed={semantics === 'navigation' ? active === tab.id : undefined}
            tabindex={semantics === 'navigation' || tabStop === tab.id ? 0 : -1}
            disabled={disabled || tab.disabled}
            onclick={(event) => void select(tab.id, event.currentTarget)}
            onfocus={(event) => reveal(event.currentTarget)}
            onkeydown={move}
        >
            {#if tab.icon}<Icon name={tab.icon} size={16} />{/if}<span class:icon-label={!!tab.icon}>{tab.label}</span>
        </button>
    {/each}
</nav>

<style>
    .tab-strip {
        display: flex;
        align-self: stretch;
        flex: 0 1 auto;
        min-width: 0;
        height: var(--density-section-toolbar);
        overflow-x: auto;
        scrollbar-width: none;
    }
    .tab-strip::-webkit-scrollbar {
        display: none;
    }
    button {
        position: relative;
        display: flex;
        flex: 0 0 auto;
        align-items: center;
        justify-content: center;
        gap: var(--density-gap);
        padding: 0 8px 2px;
        margin: 0;
        border: 0;
        border-radius: 0;
        background: transparent;
        color: var(--color-text-muted);
        font-size: var(--control-font-size);
        font-weight: 400;
        line-height: 14px;
        white-space: nowrap;
    }
    button::after {
        position: absolute;
        left: 0;
        right: 0;
        bottom: 0;
        height: 2px;
        border-radius: 0;
        background: var(--state-active-border);
        content: '';
        opacity: 0;
    }
    button:hover:not(:disabled),
    button.active {
        color: var(--color-text-strong);
        background: var(--state-hover-surface);
    }
    button.active::after {
        opacity: 1;
    }
    button:focus-visible {
        outline: 1px solid var(--state-focus);
        outline-offset: -2px;
    }
    button:disabled {
        cursor: default;
        opacity: 0.55;
    }
    [data-appearance='workspace'] {
        height: auto;
        min-height: var(--density-section-toolbar);
        gap: 3px;
    }
    [data-appearance='workspace'] button {
        padding: 0 10px 2px;
        gap: 6px;
    }
    [data-appearance='workspace'] button::after {
        background: var(--color-brand);
    }
    @media (max-width: 1200px) {
        [data-appearance='workspace'] .icon-label {
            display: none;
        }
    }
    @media (max-width: 850px) {
        [data-appearance='workspace'] button {
            padding: 0 8px 2px;
        }
    }
</style>
