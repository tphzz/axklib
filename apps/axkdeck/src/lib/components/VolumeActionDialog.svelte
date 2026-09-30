<script lang="ts">
    import type { VolumeDeletionInspection } from '../transport';
    import type { DiskTreeItem, ImageTreeAction } from '../types';
    import { modal } from '../modal';
    import type { VolumeActionPhase } from '../../features/mutation/volumeActionExecution.svelte';

    interface Props {
        action: ImageTreeAction;
        items: DiskTreeItem[];
        busy: boolean;
        phase: VolumeActionPhase;
        locked?: boolean;
        canDismiss?: boolean;
        recovery?: 'check' | 'refresh' | null;
        onrecover?: () => void;
        error: string;
        deletionInspection: VolumeDeletionInspection | null;
        oncancel: () => void;
        onsubmit: (name: string) => void;
    }

    let {
        action,
        items,
        busy,
        phase,
        locked = busy,
        canDismiss = !busy,
        recovery = null,
        onrecover = () => {},
        error,
        deletionInspection,
        oncancel,
        onsubmit,
    }: Props = $props();
    let value = $state('');
    let initialized = false;
    const item = $derived(items[0]!);
    const deletingMultiple = $derived(action === 'delete-volume' && items.length > 1);
    const deletionTargetGroups = $derived.by(() => {
        const groups = new Map<number, DiskTreeItem[]>();
        for (const target of items) {
            const partitionIndex = target.partitionIndex ?? 0;
            const targets = groups.get(partitionIndex) ?? [];
            targets.push(target);
            groups.set(partitionIndex, targets);
        }
        return [...groups].map(([partitionIndex, targets]) => ({ partitionIndex, targets }));
    });

    $effect(() => {
        if (initialized) return;
        value = action === 'rename-volume' || action === 'rename-partition' ? item.name : '';
        initialized = true;
    });

    const title = $derived(
        action === 'add-volume'
            ? 'Add volume'
            : action === 'rename-volume'
              ? 'Rename volume'
              : action === 'rename-partition'
                ? 'Rename partition'
                : deletingMultiple
                  ? `Delete ${items.length} volumes`
                  : 'Delete volume',
    );
    const subject = $derived(action === 'rename-partition' ? 'Partition' : 'Volume');
    const submitLabel = $derived(
        action === 'add-volume'
            ? 'Add'
            : action === 'rename-volume' || action === 'rename-partition'
              ? 'Rename'
              : 'Delete permanently',
    );
    const status = $derived(
        phase === 'checking'
            ? action === 'delete-volume'
                ? 'Checking object relationships...'
                : 'Checking sampler capacity...'
            : phase === 'submitting'
              ? action === 'add-volume'
                  ? 'Adding volume...'
                  : action === 'delete-volume'
                    ? 'Deleting volumes...'
                    : 'Renaming...'
              : phase === 'refreshing'
                ? 'Refreshing workspace...'
                : phase === 'checking-status'
                  ? 'Checking change status...'
                  : '',
    );
    const trimmedValue = $derived(value.trim());
    const nameValid = $derived(
        trimmedValue.length > 0 && trimmedValue.length <= 16 && /^[\x20-\x7e]+$/.test(trimmedValue),
    );
    const canSubmit = $derived(
        !locked &&
            (action === 'delete-volume'
                ? deletionInspection?.canDelete === true
                : nameValid &&
                  (action !== 'rename-volume' && action !== 'rename-partition' ? true : trimmedValue !== item.name)),
    );
    function submit(event: SubmitEvent): void {
        event.preventDefault();
        if (canSubmit) onsubmit(action === 'delete-volume' ? item.name : trimmedValue);
    }
</script>

<div class="dialog-backdrop dialog-backdrop-raised" role="presentation">
    <div
        class="dialog-shell volume-action-dialog"
        role="dialog"
        aria-modal="true"
        aria-label={title}
        use:modal={{
            onescape: () => {
                if (canDismiss) oncancel();
            },
        }}
    >
        <form class="volume-action-form" onsubmit={submit}>
            <header class="dialog-header">
                <h2>{title}</h2>
                <button class="icon-button" type="button" aria-label="Close" disabled={!canDismiss} onclick={oncancel}
                    >×</button
                >
            </header>
            <div class="volume-action-content">
                {#if action === 'delete-volume'}
                    {#if deletingMultiple}
                        <strong>Permanently delete {items.length} volumes?</strong>
                        <p>The selected volumes and all objects they contain will be destroyed.</p>
                        <div class="volume-deletion-targets">
                            {#each deletionTargetGroups as group (group.partitionIndex)}
                                <section>
                                    <h3>Partition {group.partitionIndex + 1}</h3>
                                    <ul>
                                        {#each group.targets as target (target.id)}
                                            <li>{target.name}</li>
                                        {/each}
                                    </ul>
                                </section>
                            {/each}
                        </div>
                    {:else}
                        <strong>Permanently delete “{item.name}”?</strong>
                        <p>The volume and all objects it contains will be destroyed.</p>
                    {/if}
                    <p>This action cannot be undone.</p>
                    {#if deletionInspection && !deletionInspection.canDelete}
                        <p class="dialog-error" role="alert">
                            A known object relationship crosses the volume boundary. Repair object placement from the
                            volume or partition context menu, then retry deletion.
                        </p>
                    {/if}
                {:else}
                    <label>
                        <span>{subject} name</span>
                        <input
                            class="dialog-field-control"
                            bind:value
                            data-dialog-initial-focus={action.startsWith('rename-') ? 'select' : 'caret'}
                            disabled={locked}
                            maxlength="16"
                            autocomplete="off"
                            aria-label={`${subject} name`}
                        />
                    </label>
                    {#if trimmedValue && !nameValid}
                        <p class="field-help field-help-error">Use 1–16 printable ASCII characters.</p>
                    {/if}
                {/if}
            </div>
            <footer class="dialog-footer">
                <p
                    class="dialog-footer-status"
                    class:dialog-error={Boolean(error)}
                    role={error ? 'alert' : 'status'}
                    title={error || status}
                >
                    {error || status}
                </p>
                <div class="dialog-footer-actions">
                    <button class="secondary-button" type="button" disabled={!canDismiss} onclick={oncancel}
                        >{phase === 'refresh-failed' ? 'Done' : 'Cancel'}</button
                    >
                    {#if recovery}
                        <button class="primary-button" type="button" disabled={busy} onclick={onrecover}
                            >{recovery === 'check' ? 'Check status' : 'Refresh'}</button
                        >
                    {:else}<button
                            class={action === 'delete-volume' ? 'danger-button' : 'primary-button'}
                            type="submit"
                            disabled={!canSubmit}
                        >
                            {submitLabel}
                        </button>{/if}
                </div>
            </footer>
        </form>
    </div>
</div>

<style>
    .dialog-backdrop {
        padding: 16px;
    }

    .volume-action-dialog {
        width: min(430px, 100%);
    }

    .volume-deletion-targets {
        max-height: 12rem;
        overflow: auto;
    }

    .volume-deletion-targets section + section {
        margin-top: 0.75rem;
    }

    .volume-deletion-targets h3 {
        margin: 0;
        font-size: var(--font-size-value);
    }

    .volume-deletion-targets ul {
        margin: 0.25rem 0 0;
        padding-left: 1.25rem;
    }
</style>
