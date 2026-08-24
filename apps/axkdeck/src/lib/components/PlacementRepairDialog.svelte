<script lang="ts">
    import type { PlacementRepairInspection } from '../transport';
    import type { DiskTreeItem } from '../types';
    import { modal } from '../modal';
    import Icon from './Icon.svelte';

    interface Props {
        item: DiskTreeItem;
        inspection: PlacementRepairInspection | null;
        busy: boolean;
        phase: 'inspecting' | 'idle' | 'repairing';
        error: string;
        message: string;
        oncancel: () => void;
        onsubmit: (recoveryVolumeName?: string) => void;
    }

    let { item, inspection, busy, phase, error, message, oncancel, onsubmit }: Props = $props();
    let recoveryVolumeName = $state('');
    let initializedRecoveryName = false;

    $effect(() => {
        if (initializedRecoveryName || !inspection?.recoveryVolumeName) return;
        recoveryVolumeName = inspection.recoveryVolumeName;
        initializedRecoveryName = true;
    });

    const createsRecoveryVolume = $derived(inspection?.destinations.some((entry) => entry.createsVolume) ?? false);
    const trimmedRecoveryName = $derived(recoveryVolumeName.trim());
    const recoveryNameValid = $derived(
        !createsRecoveryVolume ||
            (trimmedRecoveryName.length > 0 &&
                trimmedRecoveryName.length <= 16 &&
                trimmedRecoveryName === recoveryVolumeName &&
                /^[\x20-\x7e]+$/.test(trimmedRecoveryName)),
    );
    const canSubmit = $derived(inspection?.canRepair === true && recoveryNameValid && !busy);
    const submitLabel = $derived(phase === 'repairing' ? 'Repairing' : 'Repair placement');
    const scopeLabel = $derived(item.kind === 'partition' ? `partition “${item.name}”` : `volume “${item.name}”`);

    function objectTypeLabel(type: string, count: number): string {
        const label =
            type === 'SMPL'
                ? 'Wave Data'
                : type === 'SBNK'
                  ? 'Samples'
                  : type === 'SBAC'
                    ? 'Sample Banks'
                    : type === 'PROG'
                      ? 'Programs'
                      : type === 'SEQU'
                        ? 'Sequences'
                        : type;
        return `${count} ${label}`;
    }

    function destinationSummary(destination: PlacementRepairInspection['destinations'][number]): string {
        return Object.entries(destination.objectTypeCounts)
            .sort(([left], [right]) => left.localeCompare(right))
            .map(([type, count]) => objectTypeLabel(type, count))
            .join(', ');
    }

    function submit(): void {
        if (!canSubmit) return;
        onsubmit(createsRecoveryVolume ? trimmedRecoveryName : undefined);
    }
</script>

<div class="dialog-backdrop dialog-backdrop-raised" role="presentation">
    <div
        class="dialog-shell placement-repair-dialog"
        role="dialog"
        aria-modal="true"
        aria-label="Repair object placement"
        use:modal={{ onescape: oncancel }}
    >
        <header class="dialog-header">
            <div>
                <Icon name="layers" size={16} />
                <h2>Repair object placement</h2>
            </div>
            <button class="icon-button" type="button" aria-label="Close" disabled={busy} onclick={oncancel}>×</button>
        </header>

        <div class="placement-repair-content">
            <div>
                <strong>Inspect and repair {scopeLabel}</strong>
                <p>
                    Objects are moved only when their destination is unambiguous. Payload data and known relationships
                    are preserved.
                </p>
            </div>

            {#if phase === 'inspecting' && !inspection}
                <p role="status">Inspecting object placement…</p>
            {:else if inspection}
                {#if inspection.destinations.length > 0}
                    <section class="placement-destinations" aria-label="Repair destinations">
                        <h3>{inspection.repairObjectCount} objects can be repaired</h3>
                        {#each inspection.destinations as destination (destination.volumeName)}
                            <div class="placement-destination">
                                <span>
                                    <strong>{destination.volumeName}</strong>
                                    <small>{destinationSummary(destination)}</small>
                                </span>
                                <span>{destination.createsVolume ? 'New recovery volume' : 'Existing volume'}</span>
                            </div>
                        {/each}
                    </section>
                {:else if inspection.blockedObjectCount === 0}
                    <p class="placement-complete"><Icon name="check" size={14} /> No placement repairs are needed.</p>
                {/if}

                {#if createsRecoveryVolume}
                    <label class="recovery-volume-field">
                        <span>Recovery volume name</span>
                        <input
                            bind:value={recoveryVolumeName}
                            data-dialog-initial-focus="select"
                            disabled={busy}
                            maxlength="16"
                            autocomplete="off"
                            aria-label="Recovery volume name"
                        />
                    </label>
                    <p class="field-help">
                        Ownerless objects cannot be assigned to an existing volume safely, so they will be preserved in
                        this new volume.
                    </p>
                    {#if recoveryVolumeName && !recoveryNameValid}
                        <p class="field-help field-help-error">
                            Use 1–16 printable ASCII characters without outer spaces.
                        </p>
                    {/if}
                {/if}

                {#if inspection.blockers.length > 0}
                    <section class="placement-blockers" aria-label="Unresolved placement">
                        <h3>{inspection.blockedObjectCount} objects will remain unchanged</h3>
                        {#each inspection.blockers as blocker (blocker.code)}
                            <p>{blocker.message}</p>
                        {/each}
                    </section>
                {/if}
            {/if}

            {#if message}<p class="placement-message" role="status">{message}</p>{/if}
            {#if error}<p class="dialog-error" role="alert">{error}</p>{/if}
        </div>

        <footer class="dialog-footer">
            <button class="secondary-button" type="button" disabled={busy} onclick={oncancel}>
                {message || (inspection && !inspection.canRepair) ? 'Close' : 'Cancel'}
            </button>
            {#if !message && inspection?.canRepair}
                <button class="primary-button" type="button" disabled={!canSubmit} onclick={submit}
                    >{submitLabel}</button
                >
            {/if}
        </footer>
    </div>
</div>

<style>
    .placement-repair-dialog {
        width: min(620px, calc(100vw - 40px));
    }

    .placement-repair-content {
        min-height: 0;
        display: grid;
        gap: 12px;
        padding: 14px;
        overflow-y: auto;
    }

    .placement-repair-content strong,
    .placement-repair-content h3 {
        color: var(--color-text-strong);
    }

    .placement-repair-content p,
    .placement-repair-content h3 {
        margin: 0;
    }

    .placement-repair-content p {
        margin-top: 4px;
        font-size: var(--dialog-body-font-size);
    }

    .placement-destinations,
    .placement-blockers {
        display: grid;
        gap: 7px;
    }

    .placement-destinations h3,
    .placement-blockers h3 {
        font-size: var(--dialog-section-font-size);
    }

    .placement-destination {
        min-height: 43px;
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 12px;
        padding: 7px 9px;
        border: 1px solid var(--color-border);
        border-radius: 5px;
        background: rgb(255 255 255 / 2%);
        font-size: var(--dialog-body-font-size);
    }

    .placement-destination > span:first-child {
        min-width: 0;
        display: grid;
        gap: 2px;
    }

    .placement-destination small {
        color: var(--color-text-muted);
    }

    .placement-destination > span:last-child {
        flex: none;
        color: var(--color-text-muted);
    }

    .recovery-volume-field {
        display: grid;
        gap: 5px;
        color: var(--color-text-muted);
        font-size: var(--dialog-label-font-size);
    }

    .recovery-volume-field input {
        height: var(--density-control);
        padding: 0 8px;
        color: var(--color-text-strong);
        border: 1px solid var(--color-border);
        border-radius: 5px;
        background: var(--color-bg-deep);
    }

    .placement-blockers {
        padding: 9px;
        color: #f3c18f;
        border-left: 2px solid #d08a45;
        background: rgb(208 138 69 / 7%);
    }

    .placement-complete,
    .placement-message {
        display: flex;
        align-items: center;
        gap: 6px;
        color: #76ddb4;
    }
</style>
