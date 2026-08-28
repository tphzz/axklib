<script lang="ts">
    import { dismissAutocompleteFromOutsidePointer } from '../autocomplete';
    import { modal } from '../modal';
    import type { SampleBankAssignmentBlocker, SampleBankAssignmentOption } from '../types';
    import Icon from './Icon.svelte';

    interface Props {
        volumeName: string;
        sampleCount: number;
        options: SampleBankAssignmentOption[];
        blockers: SampleBankAssignmentBlocker[];
        busy: boolean;
        error: string;
        oncancel: () => void;
        onsubmit: (bankObjectId: string) => void;
    }

    interface AssignmentStatusSegment {
        text: string;
        warning: boolean;
    }

    let { volumeName, sampleCount, options, blockers, busy, error, oncancel, onsubmit }: Props = $props();
    let query = $state('');
    let selectedObjectId = $state('');
    let activeIndex = $state(-1);
    let listOpen = $state(false);
    let sampleBankInput = $state<HTMLInputElement>();
    const filteredOptions = $derived(
        options.filter((option) => option.name.toLocaleLowerCase().includes(query.trim().toLocaleLowerCase())),
    );
    const selected = $derived(options.find((option) => option.objectId === selectedObjectId));
    const assignmentBlocked = $derived(blockers.length > 0);
    const selectionDisabled = $derived(busy || assignmentBlocked);
    const assignmentStatusSegments = $derived.by((): AssignmentStatusSegment[] => {
        if (assignmentBlocked) return [{ text: 'Sample Bank selection unavailable', warning: false }];
        if (!selected) return [{ text: 'No Sample Bank selected', warning: false }];

        const segments: AssignmentStatusSegment[] = [];
        if (selected.movedSampleCount === 0) {
            segments.push({ text: 'No changes', warning: false });
        } else {
            if (selected.selectedMemberCount > 0) {
                segments.push({ text: `${selected.selectedMemberCount} already here`, warning: false });
            }
            if (selected.reassignedSampleCount > 0) {
                segments.push({
                    text:
                        selected.reassignedSampleCount === 1
                            ? '1 moved from another bank'
                            : `${selected.reassignedSampleCount} moved from other banks`,
                    warning: true,
                });
            }
            const linkedSampleCount = selected.movedSampleCount - selected.reassignedSampleCount;
            if (linkedSampleCount > 0) segments.push({ text: `${linkedSampleCount} linked`, warning: false });
        }
        segments.push({ text: `${selected.finalMemberCount} of 127 members`, warning: false });
        return segments;
    });
    const assignmentStatusText = $derived(assignmentStatusSegments.map((segment) => segment.text).join(' · '));
    const canSubmit = $derived(
        !busy &&
            !assignmentBlocked &&
            selected !== undefined &&
            selected.movedSampleCount > 0 &&
            selected.finalMemberCount <= 127,
    );

    function select(option: SampleBankAssignmentOption): void {
        if (option.finalMemberCount > 127 || selectionDisabled) return;
        selectedObjectId = option.objectId;
        query = option.name;
        closeList();
    }

    function updateQuery(value: string): void {
        if (selectionDisabled) return;
        query = value;
        selectedObjectId = '';
        activeIndex = 0;
        listOpen = true;
    }

    function openList(): void {
        if (selectionDisabled) return;
        listOpen = true;
        activeIndex =
            filteredOptions.length === 0 ? -1 : Math.max(0, Math.min(filteredOptions.length - 1, activeIndex));
    }

    function clearSelection(): void {
        if (selectionDisabled) return;
        query = '';
        selectedObjectId = '';
        activeIndex = -1;
        listOpen = options.length > 0;
        queueMicrotask(() => sampleBankInput?.focus());
    }

    function closeList(): void {
        listOpen = false;
        activeIndex = -1;
    }

    function handleKey(event: KeyboardEvent): void {
        if (selectionDisabled) return;
        if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
            event.preventDefault();
            listOpen = true;
            const direction = event.key === 'ArrowDown' ? 1 : -1;
            const startingIndex = activeIndex < 0 ? (direction > 0 ? -1 : filteredOptions.length) : activeIndex;
            activeIndex = Math.max(0, Math.min(filteredOptions.length - 1, startingIndex + direction));
        } else if (event.key === 'Home' || event.key === 'End') {
            event.preventDefault();
            listOpen = true;
            activeIndex = event.key === 'Home' ? 0 : Math.max(0, filteredOptions.length - 1);
        } else if (event.key === 'Enter' && listOpen && filteredOptions[activeIndex]) {
            event.preventDefault();
            select(filteredOptions[activeIndex]);
        } else if (event.key === 'Escape' && listOpen) {
            event.preventDefault();
            event.stopPropagation();
            closeList();
        }
    }

    function submit(event: SubmitEvent): void {
        event.preventDefault();
        if (canSubmit) onsubmit(selectedObjectId);
    }

    function cancel(): void {
        if (!busy) oncancel();
    }
</script>

<div class="dialog-backdrop dialog-backdrop-raised" role="presentation">
    <div
        class="dialog-shell dialog-popovers-visible assign-sample-bank-dialog"
        role="dialog"
        aria-modal="true"
        aria-label="Assign to Sample Bank"
        use:modal={{ onescape: cancel }}
    >
        <form class="assign-sample-bank-form" onsubmit={submit}>
            <header class="dialog-header">
                <h2>Assign to Sample Bank</h2>
                <button class="icon-button" type="button" aria-label="Close" disabled={busy} onclick={cancel}>
                    <Icon name="close" size={15} />
                </button>
            </header>
            <div class="assign-sample-bank-content">
                <p>
                    Assign {sampleCount} selected {sampleCount === 1 ? 'Sample' : 'Samples'} from {volumeName} to an existing
                    Sample Bank.
                </p>
                {#if blockers.length > 0}
                    <div class="assignment-blockers" role="alert">
                        <strong>Assignment is blocked</strong>
                        {#each blockers as blocker (`${blocker.sampleName}\u0000${blocker.programName}`)}
                            <p>{blocker.sampleName} is assigned directly to Program {blocker.programName}.</p>
                        {/each}
                    </div>
                {/if}
                <label for="sample-bank-search">Sample Bank</label>
                <div
                    class="sample-bank-combobox dialog-autocomplete-control"
                    use:dismissAutocompleteFromOutsidePointer={{ expanded: listOpen, ondismiss: closeList }}
                >
                    <input
                        bind:this={sampleBankInput}
                        id="sample-bank-search"
                        class="dialog-field-control"
                        value={query}
                        role="combobox"
                        aria-autocomplete="list"
                        aria-expanded={listOpen}
                        aria-controls="sample-bank-options"
                        aria-activedescendant={listOpen && filteredOptions[activeIndex]
                            ? `sample-bank-option-${filteredOptions[activeIndex].objectId}`
                            : undefined}
                        autocomplete="off"
                        disabled={selectionDisabled}
                        data-dialog-initial-focus={selectionDisabled ? undefined : 'select'}
                        oninput={(event) => updateQuery(event.currentTarget.value)}
                        onclick={openList}
                        onkeydown={handleKey}
                    />
                    {#if query.length > 0 || selected}
                        <button
                            class="dialog-autocomplete-clear"
                            type="button"
                            aria-label="Clear Sample Bank"
                            title="Clear Sample Bank"
                            disabled={selectionDisabled}
                            onclick={clearSelection}><Icon name="close" size={14} /></button
                        >
                    {/if}
                    {#if listOpen}
                        <div
                            id="sample-bank-options"
                            class="sample-bank-options dialog-autocomplete-list"
                            role="listbox"
                            aria-label="Sample Banks"
                        >
                            {#each filteredOptions as option, index (option.objectId)}
                                <button
                                    id={`sample-bank-option-${option.objectId}`}
                                    type="button"
                                    class="dialog-autocomplete-option"
                                    role="option"
                                    aria-selected={selectedObjectId === option.objectId}
                                    aria-disabled={option.finalMemberCount > 127}
                                    class:active={index === activeIndex}
                                    disabled={selectionDisabled || option.finalMemberCount > 127}
                                    onpointermove={() => (activeIndex = index)}
                                    onclick={() => select(option)}
                                >
                                    <span>{option.name}</span>
                                    <small>
                                        {option.memberCount}
                                        {option.memberCount === 1 ? 'member' : 'members'}
                                        {option.finalMemberCount > 127
                                            ? ` · ${option.finalMemberCount} after assignment exceeds 127`
                                            : ''}
                                    </small>
                                </button>
                            {:else}
                                <p class="empty-copy dialog-autocomplete-empty">No matching Sample Banks</p>
                            {/each}
                        </div>
                    {/if}
                </div>
                <div
                    class="assignment-status"
                    class:assignment-status-muted={!selected || assignmentBlocked}
                    role="status"
                    aria-live="polite"
                    title={assignmentStatusText}
                >
                    {#each assignmentStatusSegments as segment, index}
                        <span class:dialog-warning={segment.warning}>{index > 0 ? ' · ' : ''}{segment.text}</span>
                    {/each}
                </div>
                {#if error}<p class="dialog-error" role="alert">{error}</p>{/if}
            </div>
            <footer class="dialog-footer">
                <button class="secondary-button" type="button" disabled={busy} onclick={cancel}>Cancel</button>
                <button class="primary-button" type="submit" disabled={!canSubmit}>
                    {busy ? 'Assigning' : 'Assign to Sample Bank'}
                </button>
            </footer>
        </form>
    </div>
</div>

<style>
    .assign-sample-bank-dialog {
        width: min(560px, calc(100vw - 32px));
    }

    .assign-sample-bank-form {
        display: grid;
        min-height: 0;
    }

    .assign-sample-bank-content {
        display: grid;
        gap: 12px;
        padding: 16px 18px 18px;
    }

    .assign-sample-bank-content > p,
    .assignment-blockers p {
        margin: 0;
    }

    .assign-sample-bank-content > label {
        color: var(--color-text-muted);
        font-size: var(--dialog-label-font-size);
    }

    .sample-bank-options {
        position: absolute;
        z-index: 4;
        top: calc(100% + 4px);
        right: 0;
        left: 0;
    }

    .sample-bank-options :global(.dialog-autocomplete-option) {
        display: grid;
        grid-template-columns: minmax(0, 1fr) auto;
        gap: 12px;
        align-items: center;
    }

    .assignment-blockers {
        display: grid;
        gap: 6px;
        color: var(--color-danger);
    }

    .assignment-status {
        min-width: 0;
        height: 16px;
        overflow: hidden;
        color: var(--color-text);
        font-size: var(--dialog-body-font-size);
        line-height: 16px;
        text-overflow: ellipsis;
        white-space: nowrap;
    }

    .assignment-status-muted {
        color: var(--color-text-muted);
    }
</style>
