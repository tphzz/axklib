<script lang="ts">
    import { tick } from 'svelte';
    import Icon from '../../lib/components/Icon.svelte';
    import { mountEditorPopup } from './editorPopup';
    let {
        label,
        value,
        options,
        disabled = false,
        onchange,
    }: {
        label: string;
        value: number | undefined;
        options: { value: number; label: string }[];
        disabled?: boolean;
        onchange: (value: number) => void;
    } = $props();
    const id = $props.id();
    let wrapper: HTMLDivElement;
    let input: HTMLInputElement;
    let popup = $state<HTMLDivElement>();
    let open = $state(false);
    let query = $state('');
    let filtering = $state(false);
    let active = $state(-1);
    const currentLabel = $derived(options.find((item) => item.value === value)?.label ?? '');
    const filtered = $derived(
        options.filter(
            (item) =>
                !filtering ||
                !query.trim() ||
                item.label.toLowerCase().includes(query.trim().toLowerCase()) ||
                (/^\d+$/.test(query.trim()) && item.label === `CC ${query.trim().padStart(3, '0')}`),
        ),
    );
    $effect(() => {
        query = currentLabel;
        filtering = false;
    });
    $effect(() => {
        if (disabled) close();
    });
    function close() {
        open = false;
        active = -1;
        filtering = false;
        query = currentLabel;
    }
    async function reveal() {
        if (disabled) return;
        open = true;
        await tick();
        popup?.querySelectorAll('[role=option]')[active]?.scrollIntoView?.({ block: 'nearest' });
    }
    function show() {
        if (!open) {
            filtering = false;
            active = options.findIndex((item) => item.value === value);
        }
        void reveal();
    }
    function select(index: number) {
        const item = filtered[index];
        if (!item || disabled) return;
        onchange(item.value);
        close();
        input.focus();
    }
    const mount = (node: HTMLDivElement) => mountEditorPopup(node, wrapper, close);
    function key(event: KeyboardEvent) {
        if (event.key === 'Escape' && open) {
            event.preventDefault();
            event.stopPropagation();
            close();
        } else if (event.key === 'Enter' && open) {
            event.preventDefault();
            select(active);
        } else if (event.key === 'Tab') close();
        else if (['ArrowDown', 'ArrowUp', 'Home', 'End'].includes(event.key)) {
            event.preventDefault();
            if (!open) {
                filtering = false;
                active = options.findIndex((item) => item.value === value);
            }
            active = !filtered.length
                ? -1
                : event.key === 'Home'
                  ? 0
                  : event.key === 'End'
                    ? filtered.length - 1
                    : Math.max(0, Math.min(filtered.length - 1, active + (event.key === 'ArrowDown' ? 1 : -1)));
            void reveal();
        }
    }
</script>

<div class="editor-autocomplete" bind:this={wrapper}>
    <input
        class="editor-control"
        bind:this={input}
        role="combobox"
        aria-label={label}
        aria-autocomplete="list"
        aria-expanded={open}
        aria-controls={open ? id : undefined}
        aria-activedescendant={open && active >= 0 ? `${id}-${active}` : undefined}
        {disabled}
        value={query}
        placeholder={value === undefined ? 'Unavailable' : ''}
        onclick={show}
        onkeydown={key}
        oninput={(event) => {
            query = event.currentTarget.value;
            filtering = true;
            active = filtered.length ? 0 : -1;
            void reveal();
        }}
        onblur={() => {
            if (!open) query = currentLabel;
        }}
    />
    {#if query || value !== undefined}
        <button
            type="button"
            class="clear editor-icon"
            aria-label={`Clear ${label}`}
            title={`Clear ${label}`}
            {disabled}
            onclick={() => {
                query = '';
                filtering = true;
                active = -1;
                input.focus();
                void reveal();
            }}><Icon name="close" size={12} /></button
        >
    {/if}
    {#if open}
        <div {id} class="editor-select-popup" role="listbox" aria-label={label} bind:this={popup} use:mount>
            {#each filtered as item, index (item.value)}
                <div
                    id={`${id}-${index}`}
                    role="option"
                    tabindex="-1"
                    aria-selected={item.value === value}
                    class:active={active === index}
                    onpointerdown={(event) => event.preventDefault()}
                    onpointermove={() => (active = index)}
                    onclick={() => select(index)}
                    onkeydown={key}
                >
                    {item.label}
                </div>
            {:else}<div class="empty" role="status">No matches</div>{/each}
        </div>
    {/if}
</div>

<style>
    .editor-autocomplete {
        position: relative;
        min-width: 0;
    }
    input {
        padding-right: 29px !important;
    }
    .clear {
        position: absolute;
        right: 2px;
        top: 50%;
        transform: translateY(-50%);
    }
    .empty {
        padding: 5px 7px;
        color: var(--color-text-muted);
    }
</style>
