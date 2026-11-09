<script lang="ts">
    import type { ConversionReport } from '../conversions';
    let {
        report,
        acknowledged = $bindable<string[]>([]),
        disabled = false,
        preparing = false,
    }: { report: ConversionReport; acknowledged?: string[]; disabled?: boolean; preparing?: boolean } = $props();
    const losses = $derived(report.issues.filter((issue) => issue.impact === 'LOSS').map((issue) => issue.id));
    const accepted = $derived(losses.filter((id) => acknowledged.includes(id)).length);
    const checked = $derived(losses.length > 0 && accepted === losses.length);
    const mixed = $derived(accepted > 0 && !checked);
    const locked = $derived(disabled || preparing || report.fidelity === 'STRICT' || losses.length === 0);
    function toggle() {
        if (locked) return;
        const ids = new Set(losses);
        acknowledged = checked ? acknowledged.filter((id) => !ids.has(id)) : [...new Set([...acknowledged, ...losses])];
    }
</script>

<div class="loss-consent-toolbar">
    <label
        ><input
            type="checkbox"
            class="dialog-checkbox"
            disabled={locked}
            {checked}
            indeterminate={mixed}
            onchange={toggle}
        />Accept all losses</label
    >
    <span
        >{preparing
            ? 'Preparing... / controls locked'
            : disabled
              ? 'Controls locked'
              : report.fidelity === 'STRICT'
                ? `${losses.length} ${losses.length === 1 ? 'loss' : 'losses'} / Strict`
                : `${accepted} / ${losses.length} accepted`}</span
    >
</div>

<style>
    .loss-consent-toolbar {
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 4px;
        flex-wrap: wrap;
        min-height: 28px;
        font-size: var(--dialog-label-font-size);
    }
    label {
        display: flex;
        align-items: center;
        gap: 4px;
        min-height: 26px;
        font-size: var(--dialog-body-font-size);
    }
    span {
        margin-left: auto;
        color: var(--color-text-muted);
        white-space: nowrap;
    }
</style>
