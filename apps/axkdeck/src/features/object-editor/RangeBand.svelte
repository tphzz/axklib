<script lang="ts">
    import { onDestroy } from 'svelte';
    import { hoverHelp } from '../../lib/components/hoverHelp.svelte';
    let {
        low,
        high,
        min = 0,
        max = 127,
        label,
        lowLabel = '',
        highLabel = '',
        disabled = false,
        onchange,
        onbegin,
        onend,
    }: {
        low: number;
        high: number;
        min?: number;
        max?: number;
        label: string;
        lowLabel?: string;
        highLabel?: string;
        disabled?: boolean;
        onchange: (low: number, high: number) => void;
        onbegin: () => void;
        onend: () => void;
    } = $props();
    let host: HTMLDivElement;
    let cancel: (() => void) | undefined;
    const position = (value: number) => (100 * (max - value)) / (max - min);
    function change(index: number, value: number) {
        if (disabled) return;
        const next = Math.round(Math.max(index ? low : min, Math.min(index ? max : high, value)));
        onchange(index ? low : next, index ? next : high);
    }
    function drag(event: PointerEvent, index: number) {
        if (disabled) return;
        event.preventDefault();
        cancel?.();
        const target = event.currentTarget as HTMLButtonElement;
        target.focus();
        target.setPointerCapture(event.pointerId);
        onbegin();
        const move = (next: PointerEvent) => {
            const rect = host.getBoundingClientRect();
            change(index, max - ((next.clientY - rect.top) / rect.height) * (max - min));
        };
        const finish = () => cancel?.();
        cancel = () => {
            target.removeEventListener('pointermove', move);
            for (const name of ['pointerup', 'pointercancel', 'lostpointercapture'])
                target.removeEventListener(name, finish);
            onend();
            cancel = undefined;
        };
        target.addEventListener('pointermove', move);
        for (const name of ['pointerup', 'pointercancel', 'lostpointercapture']) target.addEventListener(name, finish);
    }
    onDestroy(() => cancel?.());
</script>

<div class="range-band-control" role="group" aria-label={label}>
    <span class="endpoint">{highLabel} {max}</span>
    <div class="range-track" bind:this={host}>
        <div class="range-fill" style:top={`${position(high)}%`} style:bottom={`${100 - position(low)}%`}></div>
        {#each [low, high] as value, index}
            <button
                type="button"
                role="slider"
                aria-orientation="vertical"
                aria-label={`${index ? 'High' : 'Low'} ${label.toLowerCase()} boundary`}
                aria-valuenow={value}
                aria-valuemin={index ? low : min}
                aria-valuemax={index ? max : high}
                {disabled}
                style:top={`${position(value)}%`}
                use:hoverHelp={`${index ? 'High' : 'Low'} ${label.toLowerCase()}: ${value}\nDrag vertically or use Up/Down to adjust this boundary. Shift+arrows moves by 8. Home/End selects its limits.`}
                onpointerdown={(event) => drag(event, index)}
                onkeydown={(event) => {
                    const step = event.shiftKey ? 8 : 1;
                    const next =
                        event.key === 'Home'
                            ? index
                                ? low
                                : min
                            : event.key === 'End'
                              ? index
                                  ? max
                                  : high
                              : event.key === 'ArrowUp'
                                ? value + step
                                : event.key === 'ArrowDown'
                                  ? value - step
                                  : undefined;
                    if (next === undefined) return;
                    event.preventDefault();
                    onbegin();
                    change(index, next);
                }}
                onkeyup={onend}
                onblur={onend}
            ></button>
        {/each}
    </div>
    <span class="endpoint">{lowLabel} {min}</span>
</div>

<style>
    .range-band-control {
        display: flex;
        flex-direction: column;
        align-items: center;
        gap: 7px;
        height: 112px;
        width: 62px;
        flex: 0 0 62px;
        user-select: none;
    }
    .range-track {
        position: relative;
        flex: 1;
        width: 20px;
        background: var(--color-panel-deep);
        border-inline: 1px solid var(--color-border);
    }
    .range-fill {
        position: absolute;
        inset-inline: 0;
        background: #9ce1ba45;
    }
    button {
        position: absolute;
        left: -5px;
        width: 28px;
        height: 4px;
        padding: 0;
        border: 0;
        background: var(--editor-loop);
        transform: translateY(-50%);
        cursor: ns-resize;
        touch-action: none;
    }
    button::before {
        content: '';
        position: absolute;
        inset: -6px -3px;
    }
    .endpoint {
        font: 10px var(--font-mono, monospace);
        color: var(--color-text-muted);
        white-space: nowrap;
    }
</style>
