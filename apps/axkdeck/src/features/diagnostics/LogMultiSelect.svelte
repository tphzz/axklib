<script lang="ts">
    import { onMount, tick } from 'svelte';
    import { on } from 'svelte/events';
    import Icon from '../../lib/components/Icon.svelte';
    let {
        label,
        options,
        selected,
        disabled = false,
        onchange,
    }: {
        label: string;
        options: readonly string[];
        selected: readonly string[];
        disabled?: boolean;
        onchange: (values: string[]) => void;
    } = $props();
    let open = $state(false);
    let container: HTMLDivElement;
    let trigger: HTMLButtonElement;
    const name = (value: string) => (value === 'http' ? 'HTTP' : value[0].toUpperCase() + value.slice(1));
    async function toggle(): Promise<void> {
        open = !open;
        if (open) {
            await tick();
            container.querySelector<HTMLButtonElement>('[role="menuitemcheckbox"]')?.focus();
        }
    }
    function key(event: KeyboardEvent): void {
        if (event.key === 'Escape') {
            event.preventDefault();
            open = false;
            trigger.focus();
        }
        const items = [...container.querySelectorAll<HTMLButtonElement>('[role^="menuitem"]')];
        const index = items.indexOf(document.activeElement as HTMLButtonElement);
        if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
            event.preventDefault();
            items[(index + (event.key === 'ArrowDown' ? 1 : items.length - 1)) % items.length]?.focus();
        }
        if (event.key === 'Home' || event.key === 'End') {
            event.preventDefault();
            items[event.key === 'Home' ? 0 : items.length - 1]?.focus();
        }
    }
    onMount(() => {
        const outside = on(document, 'pointerdown', (event) => {
            if (!container.contains(event.target as Node)) open = false;
        });
        const focus = on(document, 'focusin', (event) => {
            if (!container.contains(event.target as Node)) open = false;
        });
        return () => {
            outside();
            focus();
        };
    });
</script>

<div class="log-multiselect" bind:this={container}>
    <span class="log-field-label">{label}</span>
    <button
        type="button"
        class="log-filter-trigger"
        aria-label={label}
        aria-haspopup="menu"
        aria-expanded={open}
        {disabled}
        bind:this={trigger}
        onclick={toggle}
    >
        <span>{selected.length === options.length ? 'All' : `${selected.length} selected`}</span><Icon
            name="chevron"
            size={12}
        />
    </button>
    {#if open}
        <div class="log-filter-menu" role="menu" tabindex="-1" aria-label={label} onkeydown={key}>
            <div class="log-filter-actions">
                <button type="button" role="menuitem" onclick={() => onchange([...options])}>All</button>
                <button type="button" role="menuitem" onclick={() => onchange([])}>None</button>
            </div>
            {#each options as value}
                <button
                    type="button"
                    role="menuitemcheckbox"
                    aria-checked={selected.includes(value)}
                    onclick={() =>
                        onchange(
                            selected.includes(value) ? selected.filter((item) => item !== value) : [...selected, value],
                        )}
                >
                    <span class="log-filter-mark" aria-hidden="true"
                        >{#if selected.includes(value)}<Icon name="check" size={10} />{/if}</span
                    >{name(value)}
                </button>
            {/each}
        </div>
    {/if}
</div>
