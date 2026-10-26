<script lang="ts" generics="Option extends SelectOption">
    import { tick, type Snippet } from 'svelte';
    import type { SelectOption } from '../selectOptions';
    import { mountSelectPopup, popupAnchorAvailable } from '../selectPopup';
    import Icon from './Icon.svelte';

    const generatedId = $props.id();
    let {
        id = generatedId,
        label,
        labelledBy,
        describedBy,
        value,
        options,
        onchange,
        disabled = false,
        invalid = false,
        placeholder = 'Unavailable',
        emptyLabel = 'No choices available',
        title,
        class: className = '',
        size = 'default',
        initialFocus,
        renderOption,
        ontriggerkeydown,
    }: {
        id?: string;
        label?: string;
        labelledBy?: string;
        describedBy?: string;
        value: Option['value'] | undefined;
        options: readonly Option[];
        onchange: (value: Option['value']) => void;
        disabled?: boolean;
        invalid?: boolean;
        placeholder?: string;
        emptyLabel?: string;
        title?: string;
        class?: string;
        size?: 'default' | 'header' | 'toolbar';
        initialFocus?: boolean;
        renderOption?: Snippet<[Option]>;
        ontriggerkeydown?: (event: KeyboardEvent) => void;
    } = $props();
    let trigger: HTMLButtonElement;
    let popup: HTMLDivElement | undefined;
    let open = $state(false);
    let activeKey = $state<string | number>();
    let query = '';
    let typedAt = 0;
    const selectedIndex = $derived.by(() => {
        const enabledMatch = options.findIndex((item) => item.value === value && !item.disabled);
        return enabledMatch >= 0 ? enabledMatch : options.findIndex((item) => item.value === value);
    });
    const selected = $derived(options[selectedIndex]);
    const enabled = $derived(options.filter((item) => !item.disabled));
    const optionKey = (option: Option) => option.key ?? option.value;
    const activeIndex = $derived(options.findIndex((item) => optionKey(item) === activeKey && !item.disabled));
    const popupId = $derived(`${id}-listbox`);

    function close() {
        open = false;
        query = '';
    }
    function available() {
        return !disabled && popupAnchorAvailable(trigger);
    }
    function scrollActive() {
        if (activeIndex >= 0)
            popup?.querySelectorAll('[role=option]')[activeIndex]?.scrollIntoView?.({ block: 'nearest' });
    }
    async function show(initial = selected && !selected.disabled ? selected : enabled[0]) {
        if (!available()) return;
        activeKey = initial ? optionKey(initial) : undefined;
        trigger.focus({ preventScroll: true });
        open = true;
        await tick();
        if (open) scrollActive();
    }
    function select(option: Option | undefined) {
        if (!option || option.disabled || !available()) return;
        close();
        trigger.focus({ preventScroll: true });
        if (option.value !== value) onchange(option.value);
    }
    function move(key: string) {
        const index = enabled.findIndex((item) => optionKey(item) === activeKey);
        const next =
            key === 'Home'
                ? 0
                : key === 'End'
                  ? enabled.length - 1
                  : Math.max(0, Math.min(enabled.length - 1, index + (key === 'ArrowUp' ? -1 : 1)));
        activeKey = enabled[next] ? optionKey(enabled[next]) : undefined;
        void tick().then(scrollActive);
    }
    function key(event: KeyboardEvent) {
        if (open && event.key === 'Escape') {
            event.preventDefault();
            event.stopPropagation();
            close();
            return;
        }
        if (event.key === 'Tab') {
            close();
            ontriggerkeydown?.(event);
            return;
        }
        if (!open) {
            ontriggerkeydown?.(event);
            if (event.defaultPrevented) return;
        }
        if (!available() || event.ctrlKey || event.metaKey) return;
        if (['Enter', ' '].includes(event.key)) {
            event.preventDefault();
            if (open) select(options[activeIndex]);
            else void show();
        } else if (['ArrowDown', 'ArrowUp', 'Home', 'End'].includes(event.key)) {
            event.preventDefault();
            if (open) move(event.key);
            else void show(event.key === 'Home' ? enabled[0] : event.key === 'End' ? enabled.at(-1) : undefined);
        } else if (event.key.length === 1 && !event.altKey) {
            event.preventDefault();
            query = (Date.now() - typedAt > 700 ? '' : query) + event.key.toLowerCase();
            typedAt = Date.now();
            const prefix = [...query].every((character) => character === query[0]) ? query[0] : query;
            const start = prefix.length === 1 ? enabled.findIndex((item) => optionKey(item) === activeKey) + 1 : 0;
            const ordered = [...enabled.slice(start), ...enabled.slice(0, start)];
            const found = ordered.find((item) => item.label.toLowerCase().startsWith(prefix));
            if (!open) void show(found);
            else if (found) {
                activeKey = optionKey(found);
                void tick().then(scrollActive);
            }
        }
    }
    function mount(node: HTMLDivElement) {
        popup = node;
        const mounted = mountSelectPopup(node, trigger, close);
        return {
            destroy() {
                mounted.destroy();
                popup = undefined;
            },
        };
    }
    $effect(() => {
        if (disabled) close();
    });
    $effect(() => {
        if (open && activeKey !== undefined && activeIndex < 0) close();
    });
</script>

<button
    {id}
    type="button"
    class={`select-control ${className}`}
    data-size={size}
    bind:this={trigger}
    value={value === undefined ? '' : String(value)}
    role="combobox"
    aria-label={label}
    aria-labelledby={labelledBy}
    aria-describedby={describedBy}
    aria-haspopup="listbox"
    aria-expanded={open}
    aria-controls={open ? popupId : undefined}
    aria-activedescendant={open && activeIndex >= 0 ? `${popupId}-${activeIndex}` : undefined}
    aria-invalid={invalid || undefined}
    {disabled}
    data-dialog-initial-focus={initialFocus || undefined}
    title={title ?? selected?.title ?? selected?.label ?? label}
    onclick={() => (open ? close() : void show())}
    onkeydown={key}
>
    <span class="select-value"
        >{#if selected && renderOption}{@render renderOption(selected)}{:else}{selected?.label ??
                placeholder}{/if}</span
    >
    <Icon name="chevron" size={12} />
</button>
{#if open}
    <div id={popupId} class="select-popup" role="listbox" aria-label={label} aria-labelledby={labelledBy} use:mount>
        {#each options as option, index}
            <div
                id={`${popupId}-${index}`}
                role="option"
                tabindex="-1"
                aria-selected={index === selectedIndex}
                data-value={String(option.value)}
                aria-disabled={option.disabled || undefined}
                title={option.title ?? option.label}
                class:active={activeIndex === index}
                onpointerdown={(event) => event.preventDefault()}
                onpointermove={() => {
                    if (!option.disabled) activeKey = optionKey(option);
                }}
                onclick={() => select(option)}
                onkeydown={key}
            >
                <span class="select-option-label"
                    >{#if renderOption}{@render renderOption(option)}{:else}{option.label}{/if}</span
                >
                <span class="select-option-check" aria-hidden="true"
                    >{#if index === selectedIndex}<Icon name="check" size={12} />{/if}</span
                >
            </div>
        {:else}<div class="select-empty" role="status">{emptyLabel}</div>{/each}
    </div>
{/if}
