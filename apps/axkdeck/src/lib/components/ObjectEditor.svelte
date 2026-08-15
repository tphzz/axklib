<script lang="ts">
    import {
        focusCollectionIndex,
        hasDisallowedNavigationModifier,
        linearNavigationIndex,
    } from '../collectionNavigation';
    import type { InspectorSelection, ProgramSampleSelectRow } from '../types';
    import CollectionToolbar from './CollectionToolbar.svelte';

    type ProgramEditorTab = 'sample-select' | 'easy-edit' | 'effect-setup' | 'setup' | 'control';
    type SampleEditorTab = 'trim-loop' | 'map-out' | 'filter-eg' | 'lfo' | 'midi-ctrl';

    interface Tab<T extends string> {
        id: T;
        label: string;
    }

    interface Props {
        selection: InspectorSelection;
        assignmentQuery: string;
        onassignmentquerychange: (value: string) => void;
        onassignmentselect: (row: ProgramSampleSelectRow) => void;
    }

    let { selection, assignmentQuery, onassignmentquerychange, onassignmentselect }: Props = $props();
    let programTab = $state<ProgramEditorTab>('sample-select');
    let sampleTab = $state<SampleEditorTab>('trim-loop');
    let showOnlyAssigned = $state(true);

    const programTabs: Tab<ProgramEditorTab>[] = [
        { id: 'sample-select', label: 'Sample Select' },
        { id: 'easy-edit', label: 'Easy Edit' },
        { id: 'effect-setup', label: 'Effect Setup' },
        { id: 'setup', label: 'Setup' },
        { id: 'control', label: 'Control' },
    ];
    const sampleTabs: Tab<SampleEditorTab>[] = [
        { id: 'trim-loop', label: 'Trim/Loop' },
        { id: 'map-out', label: 'Map/Out' },
        { id: 'filter-eg', label: 'Filter/EG' },
        { id: 'lfo', label: 'LFO' },
        { id: 'midi-ctrl', label: 'MIDI/CTRL' },
    ];

    const normalizedAssignmentQuery = $derived(assignmentQuery.trim().toLocaleLowerCase());
    const sampleSelectRows = $derived(
        selection?.kind === 'program'
            ? showOnlyAssigned
                ? selection.sampleSelect.assigned
                : selection.sampleSelect.all
            : [],
    );
    const filteredAssignments = $derived(
        selection?.kind === 'program'
            ? normalizedAssignmentQuery
                ? sampleSelectRows.filter((row) =>
                      `${row.targetType} ${row.targetName} ${row.receiveChannelDisplays.join(' ')}`
                          .toLocaleLowerCase()
                          .includes(normalizedAssignmentQuery),
                  )
                : sampleSelectRows
            : [],
    );
    const navigableAssignments = $derived(filteredAssignments.filter((row) => row.navigable));

    function navigateAssignments(event: KeyboardEvent, row: ProgramSampleSelectRow): void {
        if (hasDisallowedNavigationModifier(event)) return;
        const currentIndex = navigableAssignments.findIndex((candidate) => candidate.id === row.id);
        const targetIndex = linearNavigationIndex(event.key, currentIndex, navigableAssignments.length);
        if (targetIndex === null) return;
        event.preventDefault();
        const target = navigableAssignments[targetIndex];
        if (target && target.id !== row.id) onassignmentselect(target);
        void focusCollectionIndex(event.currentTarget, targetIndex);
    }

    function moveTab<T extends string>(
        event: KeyboardEvent,
        tabs: Tab<T>[],
        active: T,
        select: (value: T) => void,
    ): void {
        let index = tabs.findIndex((tab) => tab.id === active);
        if (event.key === 'ArrowRight') index = (index + 1) % tabs.length;
        else if (event.key === 'ArrowLeft') index = (index - 1 + tabs.length) % tabs.length;
        else if (event.key === 'Home') index = 0;
        else if (event.key === 'End') index = tabs.length - 1;
        else return;
        event.preventDefault();
        select(tabs[index]!.id);
        queueMicrotask(() => {
            const buttons =
                event.currentTarget instanceof HTMLElement ? event.currentTarget.parentElement?.children : [];
            const button = buttons?.[index];
            if (button instanceof HTMLElement) button.focus();
        });
    }
</script>

<section class="object-editor" aria-label="Object editor" data-navigation-workspace>
    {#if selection?.kind === 'program'}
        <header class="editor-header">
            <div class="editor-tabs" role="tablist" aria-label="Program editor">
                {#each programTabs as tab (tab.id)}
                    <button
                        id={`program-tab-${tab.id}`}
                        type="button"
                        role="tab"
                        aria-selected={programTab === tab.id}
                        aria-controls={`program-panel-${tab.id}`}
                        tabindex={programTab === tab.id ? 0 : -1}
                        onclick={() => (programTab = tab.id)}
                        onkeydown={(event) => moveTab(event, programTabs, programTab, (value) => (programTab = value))}
                        >{tab.label}</button
                    >
                {/each}
            </div>
        </header>
        <div
            id={`program-panel-${programTab}`}
            class="editor-panel"
            role="tabpanel"
            aria-labelledby={`program-tab-${programTab}`}
        >
            {#if programTab === 'sample-select'}
                <CollectionToolbar
                    title="Assignments"
                    count={sampleSelectRows.length}
                    query={assignmentQuery}
                    onquerychange={onassignmentquerychange}
                    filterLabel="Show only assigned"
                    filterChecked={showOnlyAssigned}
                    onfilterchange={(checked) => (showOnlyAssigned = checked)}
                />
                <div class="editor-body" data-navigation-list>
                    <div class="assignment-table" role="table" aria-label="Program assignments">
                        <div class="assignment-header" role="row">
                            <span>Target</span><span>Rch Assign</span>
                        </div>
                        {#each filteredAssignments as row (row.id)}
                            <button
                                type="button"
                                class:unresolved={!row.navigable}
                                disabled={!row.navigable}
                                data-navigation-index={row.navigable ? navigableAssignments.indexOf(row) : undefined}
                                onclick={() => row.navigable && onassignmentselect(row)}
                                onkeydown={(event) => row.navigable && navigateAssignments(event, row)}
                            >
                                <span
                                    ><strong>{row.targetName}</strong><small
                                        >{row.navigable ? row.targetType : 'Unconfirmed assignment'}</small
                                    ></span
                                >
                                <span class="assignment-channel"
                                    >{row.receiveChannelDisplays.join(', ')}
                                    {#if row.sourceLoad}
                                        <small
                                            title="Stored CD-ROM selector; the sampler activates this assignment when it is loaded."
                                            >Source load</small
                                        >
                                    {/if}</span
                                >
                            </button>
                        {:else}
                            <p class="empty-copy">
                                {normalizedAssignmentQuery
                                    ? 'No matching Sample Banks or Samples'
                                    : showOnlyAssigned
                                      ? 'No assigned Sample Banks or Samples'
                                      : 'No Sample Banks or Samples'}
                            </p>
                        {/each}
                    </div>
                </div>
            {:else}
                <div class="editor-canvas"></div>
            {/if}
        </div>
    {:else if selection?.kind === 'sample'}
        <header class="editor-header">
            <div class="editor-object-title"><span>Sample</span><strong>{selection.item.name}</strong></div>
            <div class="editor-tabs" role="tablist" aria-label="Sample editor">
                {#each sampleTabs as tab (tab.id)}
                    <button
                        id={`sample-tab-${tab.id}`}
                        type="button"
                        role="tab"
                        aria-selected={sampleTab === tab.id}
                        aria-controls={`sample-panel-${tab.id}`}
                        tabindex={sampleTab === tab.id ? 0 : -1}
                        onclick={() => (sampleTab = tab.id)}
                        onkeydown={(event) => moveTab(event, sampleTabs, sampleTab, (value) => (sampleTab = value))}
                        >{tab.label}</button
                    >
                {/each}
            </div>
        </header>
        <div
            id={`sample-panel-${sampleTab}`}
            class="editor-panel editor-canvas"
            role="tabpanel"
            aria-labelledby={`sample-tab-${sampleTab}`}
        ></div>
    {:else if selection?.kind === 'sample-bank'}
        <div class="editor-placeholder"><p class="empty-copy">Sample Bank editor unavailable</p></div>
    {:else if selection?.kind === 'wave-data'}
        <div class="editor-placeholder"><p class="empty-copy">Wave Data editor unavailable</p></div>
    {:else}
        <div class="editor-placeholder"><p class="empty-copy">No object selected</p></div>
    {/if}
</section>
