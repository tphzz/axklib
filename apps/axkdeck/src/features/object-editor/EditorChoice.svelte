<script lang="ts">
    import { tick } from 'svelte';
    import { measureWidth } from './measureWidth';
    import { mountEditorPopup } from './editorPopup';
    import Icon from '../../lib/components/Icon.svelte';
    import EditorOptionLabel from './EditorOptionLabel.svelte';
    import { optionHelp, type EditorOption } from './editorOptions';
    let {
        label,
        value,
        options,
        disabled = false,
        segmented,
        onchange,
        ontriggerkeydown,
    }: {
        label: string;
        value: number | undefined;
        options: EditorOption[];
        disabled?: boolean;
        segmented?: boolean;
        onchange: (value: number) => void;
        ontriggerkeydown?: (event: KeyboardEvent) => void;
    } = $props();
    let trigger = $state<HTMLButtonElement>();
    let popup: HTMLDivElement | undefined;
    let open = $state(false);
    let active = $state(0);
    let query = '';
    let typedAt = 0;
    let width = $state(0);
    let minimumWidth = $state(0);
    function measureLabels(node: HTMLElement) {
        return measureWidth(node, {
            scope: 'labels',
            change: () => {
                minimumWidth =
                    Math.max(0, ...Array.from(node.children, (child) => (child as HTMLElement).offsetWidth)) *
                        options.length +
                    (options.length - 1) * 2 +
                    6;
            },
        });
    }
    const showSegments = $derived(segmented ?? (options.length <= 4 && (!width || width >= minimumWidth)));
    const measure = $derived(segmented === undefined && options.length <= 4);
    const selected = $derived(options.find((option) => option.value === value));
    function observeChoice(node: HTMLElement, enabled: boolean) {
        let observer: ReturnType<typeof measureWidth> | undefined;
        const update = (active: boolean) => {
            observer?.destroy();
            observer = active ? measureWidth(node, { scope: 'choice', change: (value) => (width = value) }) : undefined;
        };
        update(enabled);
        return { update, destroy: () => observer?.destroy() };
    }
    const id = $props.id();
    function close(focus = false) {
        open = false;
        if (focus) trigger?.focus();
    }
    async function show() {
        if (disabled) return;
        active = Math.max(
            0,
            options.findIndex((option) => option.value === value),
        );
        open = true;
        await tick();
        popup?.focus();
        scrollActive();
    }
    function scrollActive() {
        popup?.querySelectorAll('[role=option]')[active]?.scrollIntoView({ block: 'nearest' });
    }
    function select(index: number) {
        if (disabled || !options[index] || options[index]!.disabled) return;
        onchange(options[index]!.value);
        close(true);
    }
    function key(event: KeyboardEvent) {
        if (event.key === 'Escape') {
            event.preventDefault();
            event.stopPropagation();
            close(true);
            return;
        }
        if (event.key === 'Tab') {
            close(true);
            return;
        }
        if (event.key === 'Enter' || event.key === ' ') {
            event.preventDefault();
            select(active);
            return;
        }
        if (event.key === 'ArrowDown' || event.key === 'ArrowRight') active = (active + 1) % options.length;
        else if (event.key === 'ArrowUp' || event.key === 'ArrowLeft')
            active = (active - 1 + options.length) % options.length;
        else if (event.key === 'Home') active = 0;
        else if (event.key === 'End') active = options.length - 1;
        else if (event.key.length === 1 && !event.ctrlKey && !event.altKey && !event.metaKey) {
            query = (Date.now() - typedAt > 700 ? '' : query) + event.key.toLowerCase();
            typedAt = Date.now();
            const found = options.findIndex((option) => option.label.toLowerCase().startsWith(query));
            if (found >= 0) active = found;
        } else return;
        event.preventDefault();
        void tick().then(scrollActive);
    }
    function mount(node: HTMLDivElement) {
        popup = node;
        if (!trigger) return;
        const mounted = mountEditorPopup(node, trigger, close);
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
</script>

<div class="choice-control" use:observeChoice={measure}>
    {#if measure}<div class="choice-measurement" aria-hidden="true" use:measureLabels>
            {#each options as option}<EditorOptionLabel {...option} />{/each}
        </div>{/if}
    {#if showSegments}
        <div class="editor-choices" role="group" aria-label={label}>
            {#each options as option, index}
                <button
                    type="button"
                    class="editor-choice"
                    aria-pressed={value === option.value}
                    aria-label={`${label}: ${option.label}`}
                    title={optionHelp(option)}
                    disabled={disabled || option.disabled}
                    onclick={() => onchange(option.value)}
                    onkeydown={(event) => {
                        let next = index;
                        if (event.key === 'ArrowRight') next = (index + 1) % options.length;
                        else if (event.key === 'ArrowLeft') next = (index - 1 + options.length) % options.length;
                        else if (event.key === 'Home') next = 0;
                        else if (event.key === 'End') next = options.length - 1;
                        else return;
                        event.preventDefault();
                        for (let attempt = 0; options[next]!.disabled && attempt < options.length; attempt++)
                            next =
                                (next + (event.key === 'ArrowLeft' || event.key === 'End' ? options.length - 1 : 1)) %
                                options.length;
                        if (options[next]!.disabled) return;
                        onchange(options[next]!.value);
                        (event.currentTarget.parentElement?.children[next] as HTMLButtonElement)?.focus();
                    }}><EditorOptionLabel {...option} /></button
                >
            {/each}
        </div>
    {:else}
        <button
            type="button"
            class="editor-control editor-select"
            bind:this={trigger}
            aria-label={label}
            aria-haspopup="listbox"
            aria-expanded={open}
            aria-controls={open ? id : undefined}
            title={selected ? optionHelp(selected) : label}
            {disabled}
            onclick={() => (open ? close() : void show())}
            onkeydown={(event) => {
                ontriggerkeydown?.(event);
                if (event.defaultPrevented) return;
                if (['ArrowDown', 'ArrowUp', 'Home', 'End'].includes(event.key)) {
                    event.preventDefault();
                    void show();
                }
            }}
        >
            <EditorOptionLabel
                label={selected?.label ?? 'Unavailable'}
                extended={selected?.extended}
                a5000Only={selected?.a5000Only}
            /><Icon name="chevron" size={12} />
        </button>
        {#if open}
            <div
                {id}
                class="editor-select-popup"
                role="listbox"
                aria-label={label}
                tabindex="-1"
                aria-activedescendant={`${id}-${active}`}
                use:mount
                onkeydown={key}
            >
                {#each options as option, index}
                    <div
                        id={`${id}-${index}`}
                        role="option"
                        tabindex="-1"
                        aria-selected={option.value === value}
                        aria-disabled={option.disabled || undefined}
                        title={optionHelp(option)}
                        class:active={active === index}
                        onpointermove={() => (active = index)}
                        onkeydown={key}
                        onclick={() => select(index)}
                    >
                        <EditorOptionLabel {...option} />
                    </div>
                {/each}
            </div>
        {/if}
    {/if}
</div>

<style>
    .choice-control {
        position: relative;
        min-width: 0;
        width: 100%;
    }
    .choice-measurement {
        position: absolute;
        visibility: hidden;
        pointer-events: none;
        display: flex;
        width: max-content;
        height: 0;
        overflow: hidden;
        font-size: 11px;
    }
    .choice-measurement > :global(.editor-option-label) {
        padding-inline: 7px;
        max-width: none;
    }
</style>
