<script lang="ts">
    import { tick } from 'svelte';
    import type { InterfaceScaleMode, InterfaceScaleState } from '../interfaceScale';
    import Icon from './Icon.svelte';

    interface Props {
        libraryOpen: boolean;
        editorOpen: boolean;
        editorAvailable: boolean;
        inspectorOpen: boolean;
        interfaceScale?: InterfaceScaleState | null;
        ontogglelibrary: () => void;
        ontoggleeditor: () => void;
        ontoggleinspector: () => void;
        oninterfacescalechange?: (mode: InterfaceScaleMode) => void;
    }

    let {
        libraryOpen,
        editorOpen,
        editorAvailable,
        inspectorOpen,
        interfaceScale = null,
        ontogglelibrary,
        ontoggleeditor,
        ontoggleinspector,
        oninterfacescalechange = () => undefined,
    }: Props = $props();
    let scaleMenuOpen = $state(false);
    let scaleMenu: HTMLElement | undefined = $state();
    let scaleTrigger: HTMLButtonElement | undefined = $state();
    const scaleOptions: readonly { mode: InterfaceScaleMode; label: string }[] = [
        { mode: '1', label: '100%' },
        { mode: '1.25', label: '125%' },
        { mode: '1.5', label: '150%' },
    ];
    const effectivePercent = $derived(Math.round((interfaceScale?.appliedZoom ?? 1) * 100));
    const scaleLabel = $derived(
        interfaceScale?.mode === 'auto'
            ? `Interface scale, Auto, ${effectivePercent}%`
            : `Interface scale, ${Math.round(Number(interfaceScale?.mode ?? 1) * 100)}%`,
    );

    function chooseScale(mode: InterfaceScaleMode): void {
        scaleMenuOpen = false;
        oninterfacescalechange(mode);
    }

    async function openScaleMenu(focusLast = false): Promise<void> {
        scaleMenuOpen = true;
        await tick();
        const items = Array.from(scaleMenu?.querySelectorAll<HTMLButtonElement>('[role="menuitemradio"]') ?? []);
        const selected = items.find((item) => item.getAttribute('aria-checked') === 'true');
        (focusLast ? items.at(-1) : (selected ?? items[0]))?.focus();
    }

    function closeScaleMenu(restoreFocus = false): void {
        scaleMenuOpen = false;
        if (restoreFocus) scaleTrigger?.focus();
    }

    function handleScaleTriggerKeydown(event: KeyboardEvent): void {
        if (event.key !== 'ArrowDown' && event.key !== 'ArrowUp') return;
        event.preventDefault();
        void openScaleMenu(event.key === 'ArrowUp');
    }

    function navigateScaleMenu(event: KeyboardEvent): void {
        if (event.key === 'Escape') {
            event.preventDefault();
            event.stopPropagation();
            closeScaleMenu(true);
            return;
        }
        if (!['ArrowDown', 'ArrowUp', 'Home', 'End'].includes(event.key)) return;
        const items = Array.from(scaleMenu?.querySelectorAll<HTMLButtonElement>('[role="menuitemradio"]') ?? []);
        if (items.length === 0) return;
        event.preventDefault();
        const activeIndex = items.indexOf(document.activeElement as HTMLButtonElement);
        const nextIndex =
            event.key === 'Home'
                ? 0
                : event.key === 'End'
                  ? items.length - 1
                  : event.key === 'ArrowDown'
                    ? (activeIndex + 1) % items.length
                    : (activeIndex - 1 + items.length) % items.length;
        items[nextIndex]?.focus();
    }
</script>

<svelte:window
    onclick={() => closeScaleMenu()}
    onkeydown={(event) => event.key === 'Escape' && scaleMenuOpen && closeScaleMenu(true)}
/>

<div class="layout-controls" role="toolbar" aria-label="Panel layout">
    {#if interfaceScale}
        <div class="interface-scale">
            <button
                bind:this={scaleTrigger}
                class:active={scaleMenuOpen}
                class="icon-button"
                type="button"
                aria-label={scaleLabel}
                aria-haspopup="menu"
                aria-expanded={scaleMenuOpen}
                title={scaleLabel}
                onclick={(event) => {
                    event.stopPropagation();
                    scaleMenuOpen = !scaleMenuOpen;
                }}
                onkeydown={handleScaleTriggerKeydown}
            >
                <Icon name="sliders" size={17} />
            </button>
            {#if scaleMenuOpen}
                <div
                    bind:this={scaleMenu}
                    class="interface-scale-menu"
                    role="menu"
                    aria-label="Interface scale"
                    tabindex="-1"
                    onclick={(event) => event.stopPropagation()}
                    onkeydown={navigateScaleMenu}
                >
                    <button
                        type="button"
                        role="menuitemradio"
                        aria-checked={interfaceScale.mode === 'auto'}
                        onclick={() => chooseScale('auto')}>Auto</button
                    >
                    {#each scaleOptions as option}
                        <button
                            type="button"
                            role="menuitemradio"
                            aria-checked={interfaceScale.mode === option.mode}
                            onclick={() => chooseScale(option.mode)}>{option.label}</button
                        >
                    {/each}
                </div>
            {/if}
        </div>
    {/if}
    <button
        class:active={libraryOpen}
        class="icon-button"
        type="button"
        aria-label="Library panel"
        aria-pressed={libraryOpen}
        title="Library panel"
        onclick={ontogglelibrary}
    >
        <Icon name="panel-left" size={17} />
    </button>
    <button
        class:active={editorOpen}
        class="icon-button"
        type="button"
        aria-label="Editor panel"
        aria-pressed={editorOpen}
        aria-disabled={!editorAvailable}
        disabled={!editorAvailable}
        title="Editor panel"
        onclick={ontoggleeditor}
    >
        <Icon name="panel-bottom" size={17} />
    </button>
    <button
        class:active={inspectorOpen}
        class="icon-button inspector-toggle"
        type="button"
        aria-label="Inspector panel"
        aria-pressed={inspectorOpen}
        title="Inspector panel"
        onclick={ontoggleinspector}
    >
        <Icon name="panel-right" size={17} />
    </button>
</div>

<style>
    .layout-controls {
        display: flex;
        flex: none;
        align-items: center;
        gap: 2px;
        padding-left: 10px;
        border-left: 1px solid var(--color-border);
    }

    .interface-scale {
        position: relative;
    }

    .interface-scale-menu {
        position: absolute;
        top: calc(100% + 5px);
        right: 0;
        z-index: 30;
        display: grid;
        width: 118px;
        padding: 4px;
        border: 1px solid var(--color-border);
        border-radius: 5px;
        background: var(--color-panel);
        box-shadow: 0 10px 24px rgb(0 0 0 / 35%);
    }

    .interface-scale-menu button {
        display: flex;
        height: 27px;
        align-items: center;
        padding: 0 8px;
        color: var(--color-text);
        border: 0;
        border-radius: 3px;
        background: transparent;
        font-size: 10px;
        cursor: pointer;
    }

    .interface-scale-menu button:hover,
    .interface-scale-menu button:focus-visible {
        background: var(--color-panel-raised);
        outline: none;
    }

    .interface-scale-menu button[aria-checked='true'] {
        color: var(--color-text-strong);
        background: rgb(104 151 187 / 18%);
    }

    @media (max-width: 850px) {
        .inspector-toggle {
            display: none;
        }
    }
</style>
