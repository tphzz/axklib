<script lang="ts">
    import { onDestroy } from 'svelte';
    import { on } from 'svelte/events';

    let {
        label,
        description,
        contextKey = '',
    }: {
        label: string;
        description: string;
        contextKey?: string;
    } = $props();
    const id = $props.id();
    let trigger = $state<HTMLButtonElement>();
    let tooltip: HTMLElement | undefined;
    let open = $state(false);
    let pinned = false;
    let timer: ReturnType<typeof setTimeout> | undefined;

    function cancelTimer() {
        clearTimeout(timer);
        timer = undefined;
    }
    function close() {
        cancelTimer();
        pinned = false;
        open = false;
    }
    function show() {
        cancelTimer();
        open = true;
    }
    function enter() {
        cancelTimer();
        if (!open) timer = setTimeout(show, 400);
    }
    function leave() {
        cancelTimer();
        if (!pinned && document.activeElement !== trigger) timer = setTimeout(close, 150);
    }
    function toggle() {
        if (pinned) close();
        else {
            pinned = true;
            show();
        }
    }

    function mountTooltip(node: HTMLElement) {
        if (!trigger) return;
        tooltip = node;
        document.body.appendChild(node);
        const anchor = trigger.getBoundingClientRect();
        const bodyScale = document.body.offsetWidth
            ? document.body.getBoundingClientRect().width / document.body.offsetWidth
            : 1;
        const anchorScale = trigger.offsetWidth ? anchor.width / trigger.offsetWidth : 1;
        const scale = anchorScale || 1;
        node.style.zoom = String(scale / (bodyScale || 1));
        node.style.width = `${Math.min(320, (window.innerWidth - 16) / scale)}px`;
        node.style.maxHeight = `${(window.innerHeight - 16) / scale}px`;
        const bounds = node.getBoundingClientRect();
        const left = Math.max(8, Math.min(anchor.left, window.innerWidth - bounds.width - 8));
        const below = anchor.bottom + 6;
        const top =
            below + bounds.height <= window.innerHeight - 8 ? below : Math.max(8, anchor.top - bounds.height - 6);
        node.style.left = `${left / scale}px`;
        node.style.top = `${top / scale}px`;
        return {
            destroy() {
                node.remove();
                tooltip = undefined;
            },
        };
    }

    $effect(() => {
        contextKey;
        label;
        description;
        close();
    });
    $effect(() => {
        if (!open) return;
        const release = [
            on(
                window,
                'pointerdown',
                (event) => {
                    if (
                        event.target instanceof Node &&
                        !trigger?.contains(event.target) &&
                        !tooltip?.contains(event.target)
                    )
                        close();
                },
                { capture: true },
            ),
            on(
                window,
                'keydown',
                (event) => {
                    if (event.key === 'Escape') {
                        event.stopPropagation();
                        close();
                    }
                },
                { capture: true },
            ),
            on(window, 'focusin', (event) => {
                if (
                    event.target instanceof Node &&
                    !trigger?.contains(event.target) &&
                    !tooltip?.contains(event.target)
                )
                    close();
            }),
            on(window, 'resize', close),
            on(
                window,
                'scroll',
                (event) => {
                    if (event.target !== tooltip) close();
                },
                { capture: true },
            ),
        ];
        return () => release.forEach((remove) => remove());
    });
    onDestroy(cancelTimer);
</script>

{#if description}
    <button
        type="button"
        class="attribute-help-label"
        bind:this={trigger}
        aria-describedby={open ? id : undefined}
        onpointerenter={enter}
        onpointerleave={leave}
        onfocus={show}
        onblur={() => {
            if (!pinned) close();
        }}
        onclick={toggle}>{label}</button
    >
    {#if open}
        <div
            {id}
            role="tooltip"
            class="attribute-help-tooltip"
            use:mountTooltip
            onpointerenter={cancelTimer}
            onpointerleave={leave}
        >
            {description}
        </div>
    {/if}
{:else}{label}{/if}

<style>
    .attribute-help-label {
        all: unset;
        display: inline;
        cursor: help;
        color: inherit;
        font: inherit;
        text-align: left;
        overflow-wrap: anywhere;
    }
    .attribute-help-label:focus-visible {
        outline: 1px solid var(--color-text-muted);
        outline-offset: 2px;
    }
    .attribute-help-tooltip {
        position: fixed;
        z-index: 2000;
        box-sizing: border-box;
        padding: 6px 8px;
        border: 1px solid var(--color-border);
        border-radius: 5px;
        background: var(--color-panel-raised);
        color: var(--color-text);
        box-shadow: 0 8px 20px rgb(0 0 0 / 35%);
        font-family: var(--font-sans);
        font-size: 11px;
        line-height: 1.45;
        white-space: pre-line;
        overflow-wrap: anywhere;
        overflow: auto;
    }
</style>
