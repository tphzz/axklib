<script lang="ts">
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

    let { volumeName, sampleCount, options, blockers, busy, error, oncancel, onsubmit }: Props = $props();
    let query = $state('');
    let selectedObjectId = $state('');
    let activeIndex = $state(0);
    let listOpen = $state(true);
    const filteredOptions = $derived(
        options.filter((option) => option.name.toLocaleLowerCase().includes(query.trim().toLocaleLowerCase())),
    );
    const selected = $derived(options.find((option) => option.objectId === selectedObjectId));
    const canSubmit = $derived(
        !busy &&
            blockers.length === 0 &&
            selected !== undefined &&
            selected.movedSampleCount > 0 &&
            selected.finalMemberCount <= 127,
    );

    function select(option: SampleBankAssignmentOption): void {
        if (option.finalMemberCount > 127 || busy) return;
        selectedObjectId = option.objectId;
        query = option.name;
        listOpen = false;
    }

    function updateQuery(value: string): void {
        query = value;
        selectedObjectId = '';
        activeIndex = 0;
        listOpen = true;
    }

    function handleKey(event: KeyboardEvent): void {
        if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
            event.preventDefault();
            listOpen = true;
            const direction = event.key === 'ArrowDown' ? 1 : -1;
            activeIndex = Math.max(0, Math.min(filteredOptions.length - 1, activeIndex + direction));
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
            listOpen = false;
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
        class="dialog-shell assign-sample-bank-dialog"
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
                <div class="sample-bank-combobox">
                    <input
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
                        disabled={busy}
                        data-dialog-initial-focus="select"
                        oninput={(event) => updateQuery(event.currentTarget.value)}
                        onfocus={() => (listOpen = true)}
                        onkeydown={handleKey}
                    />
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
                                    disabled={busy || option.finalMemberCount > 127}
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
                {#if selected}
                    <div class="assignment-summary" aria-live="polite">
                        {#if selected.selectedMemberCount > 0}
                            <p>
                                {selected.selectedMemberCount} selected
                                {selected.selectedMemberCount === 1 ? 'Sample is' : 'Samples are'} already in this Sample
                                Bank and will remain in place.
                            </p>
                        {/if}
                        {#if selected.movedSampleCount > 0}
                            {#if selected.reassignedSampleCount > 0}
                                <p class="dialog-warning">
                                    {selected.reassignedSampleCount} selected
                                    {selected.reassignedSampleCount === 1 ? 'Sample will' : 'Samples will'} be detached from
                                    {selected.reassignedSampleCount === 1
                                        ? 'its current Sample Bank'
                                        : 'their current Sample Banks'} and appended.
                                </p>
                            {/if}
                            {#if selected.movedSampleCount > selected.reassignedSampleCount}
                                <p>
                                    {selected.movedSampleCount - selected.reassignedSampleCount} unassigned selected
                                    {selected.movedSampleCount - selected.reassignedSampleCount === 1
                                        ? 'Sample will be'
                                        : 'Samples will be'} linked and appended.
                                </p>
                            {/if}
                        {:else}
                            <p>All selected Samples are already in this Sample Bank. No changes are needed.</p>
                        {/if}
                        <p>{selected.finalMemberCount} of 127 members after assignment</p>
                    </div>
                {/if}
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
    .assignment-blockers p,
    .assignment-summary p {
        margin: 0;
    }

    .assign-sample-bank-content > label {
        color: var(--color-text-muted);
        font-size: var(--dialog-label-font-size);
    }

    .sample-bank-combobox {
        position: relative;
    }

    .sample-bank-combobox > input {
        width: 100%;
    }

    .sample-bank-options {
        margin-top: 4px;
    }

    .sample-bank-options :global(.dialog-autocomplete-option) {
        display: grid;
        grid-template-columns: minmax(0, 1fr) auto;
        gap: 12px;
        align-items: center;
        min-height: 38px;
        padding-top: 4px;
        padding-bottom: 4px;
    }

    .assignment-blockers,
    .assignment-summary {
        display: grid;
        gap: 6px;
    }

    .assignment-blockers {
        color: var(--color-danger);
    }
</style>
