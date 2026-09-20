<script lang="ts">
    import { onDestroy } from 'svelte';
    let {
        label,
        value,
        min = 0,
        max = 127,
        step = 1,
        scale = 1,
        offset = 0,
        unit = '',
        disabled = false,
        slider = true,
        resetValue,
        onchange,
        onbegin = () => {},
        onend = () => {},
        oninvalid = () => {},
    }: {
        label: string;
        value: number | undefined;
        min?: number;
        max?: number;
        step?: number;
        scale?: number;
        offset?: number;
        unit?: string;
        disabled?: boolean;
        slider?: boolean;
        resetValue?: number;
        onchange: (value: number) => void;
        onbegin?: () => void;
        onend?: () => void;
        oninvalid?: (message: string) => void;
    } = $props();
    let text = $state('');
    let error = $state('');
    const display = (next: number) =>
        String(
            Number(((next - offset) / scale).toFixed(Math.min(6, Math.max(0, Math.ceil(Math.log10(scale / step)))))),
        );
    $effect(() => {
        text = value === undefined ? '' : display(value);
    });
    function invalid(message: string) {
        error = message;
        oninvalid(message);
    }
    function edit(event: Event) {
        const input = event.currentTarget as HTMLInputElement;
        text = input.value;
        const next = Math.round((input.valueAsNumber * scale + offset) / step) * step;
        if (!text || !Number.isFinite(next) || next < min || next > max) {
            invalid(`${label}: enter ${(min - offset) / scale} to ${(max - offset) / scale}${unit ? ` ${unit}` : ''}`);
            return;
        }
        invalid('');
        onchange(next);
    }
    function reset() {
        if (disabled || resetValue === undefined) return;
        invalid('');
        onchange(Math.max(min, Math.min(max, resetValue)));
    }
    function finish() {
        onend();
        if (error) {
            text = value === undefined ? '' : display(value);
            invalid('');
        }
    }
    onDestroy(() => {
        onend();
        oninvalid('');
    });
</script>

<div class="editor-number" class:bipolar={min < 0 && max > 0}>
    {#if slider}
        <input
            class="editor-slider"
            type="range"
            aria-label={`${label} slider`}
            {min}
            {max}
            {step}
            value={value ?? min}
            disabled={disabled || value === undefined || min >= max}
            title={resetValue === undefined
                ? label
                : `${label}. Double-click or Alt+Backspace to restore the saved value.`}
            onpointerdown={onbegin}
            onpointerup={onend}
            onpointercancel={onend}
            onlostpointercapture={onend}
            onkeydown={(event) => {
                if (event.altKey && event.key === 'Backspace') {
                    event.preventDefault();
                    reset();
                } else if (
                    ['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown', 'Home', 'End', 'PageUp', 'PageDown'].includes(
                        event.key,
                    )
                )
                    onbegin();
            }}
            onkeyup={onend}
            onblur={onend}
            ondblclick={reset}
            oninput={(event) => {
                invalid('');
                onchange(Number(event.currentTarget.value));
            }}
        />
    {/if}
    <div
        class="editor-value"
        class:has-unit={!!unit}
        style:--editor-unit-width={`${Math.max(30, unit.length * 6 + 8)}px`}
    >
        <input
            class="editor-control"
            type="number"
            aria-label={label}
            aria-invalid={!!error}
            title={error || label}
            min={(min - offset) / scale}
            max={(max - offset) / scale}
            step={step / scale}
            value={text}
            placeholder="Unavailable"
            disabled={disabled || value === undefined}
            oninput={edit}
            onblur={finish}
            onkeydown={(event) => {
                if (event.key === 'Escape') {
                    event.stopPropagation();
                    finish();
                }
                if (event.altKey && event.key === 'Backspace') {
                    event.preventDefault();
                    reset();
                }
            }}
        />
        {#if unit}<span>{unit}</span>{/if}
    </div>
</div>
