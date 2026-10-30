<script lang="ts">
    let {
        checked,
        label,
        disabled = false,
        density = 'editor',
        showLabel = false,
        onchange,
    }: {
        checked: boolean;
        label: string;
        disabled?: boolean;
        density?: 'editor' | 'diagnostic';
        showLabel?: boolean;
        onchange: (next: boolean) => void | Promise<void>;
    } = $props();
</script>

<button
    type="button"
    role="switch"
    class="switch-control"
    class:labelled={showLabel}
    data-density={density}
    aria-label={label}
    aria-checked={checked}
    {disabled}
    onclick={() => {
        if (!disabled) void onchange(!checked);
    }}
>
    <span class="switch-track" aria-hidden="true"><span class="switch-thumb"></span></span>
    {#if showLabel}{label}{/if}
</button>

<style>
    .switch-control {
        display: inline-flex;
        align-items: center;
        justify-content: center;
        flex: none;
        gap: 5px;
        width: 30px;
        height: 17px;
        margin: 0;
        padding: 0;
        color: var(--color-text);
        border: 0;
        border-radius: 10px;
        background: transparent;
        font: inherit;
        font-size: 10px;
        white-space: nowrap;
        cursor: pointer;
    }
    .switch-track {
        display: block;
        position: relative;
        flex: none;
        width: 30px;
        height: 17px;
        border: 1px solid var(--color-control-border);
        border-radius: 10px;
        background: var(--color-bg-deep);
    }
    .switch-thumb {
        position: absolute;
        top: 2px;
        left: 2px;
        width: 11px;
        height: 11px;
        border-radius: 50%;
        background: var(--color-text-muted);
    }
    [aria-checked='true'] .switch-track {
        background: var(--color-accent);
    }
    [aria-checked='true'] .switch-thumb {
        left: 14px;
        background: var(--color-action-text);
    }
    [data-density='diagnostic'],
    [data-density='diagnostic'] .switch-track {
        width: 26px;
        height: 14px;
    }
    [data-density='diagnostic'] .switch-thumb {
        width: 8px;
        height: 8px;
    }
    .switch-control.labelled {
        width: auto;
        height: auto;
        min-height: 17px;
        border-radius: 3px;
    }
    .switch-control:focus-visible {
        outline: 2px solid var(--state-focus);
        outline-offset: 2px;
    }
    .switch-control:disabled {
        opacity: 0.45;
        cursor: default;
    }
    @media (forced-colors: active) {
        .switch-track {
            border-color: ButtonText;
        }
        [aria-checked='true'] .switch-track {
            background: Highlight;
            forced-color-adjust: none;
        }
        .switch-thumb {
            background: ButtonText;
        }
        [aria-checked='true'] .switch-thumb {
            background: HighlightText;
        }
        .switch-control:focus-visible {
            outline-color: Highlight;
        }
    }
</style>
