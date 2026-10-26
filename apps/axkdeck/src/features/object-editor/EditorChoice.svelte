<script lang="ts">
    import { measureWidth } from './measureWidth';
    import SelectControl from '../../lib/components/SelectControl.svelte';
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
    const choiceOptions = $derived(options.map((option) => ({ ...option, title: optionHelp(option) })));
    function observeChoice(node: HTMLElement, enabled: boolean) {
        let observer: ReturnType<typeof measureWidth> | undefined;
        const update = (active: boolean) => {
            observer?.destroy();
            observer = active ? measureWidth(node, { scope: 'choice', change: (value) => (width = value) }) : undefined;
        };
        update(enabled);
        return { update, destroy: () => observer?.destroy() };
    }
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
        <SelectControl
            class="editor-control editor-select"
            {label}
            {value}
            options={choiceOptions}
            {onchange}
            {ontriggerkeydown}
            title={selected ? optionHelp(selected) : label}
            {disabled}
        >
            {#snippet renderOption(option)}<EditorOptionLabel {...option} />{/snippet}
        </SelectControl>
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
