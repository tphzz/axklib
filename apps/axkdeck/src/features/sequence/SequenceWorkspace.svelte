<script lang="ts">
    import {
        emptyPackageExportSelection,
        selectionMode,
        type ObjectSelectionMode,
        updatePackageExportSelection,
        type PackageExportSelectionState,
    } from '../../lib/objectSelection';
    import {
        collectionPageStep,
        focusCollectionIndex,
        hasDisallowedNavigationModifier,
        keyboardSelectionMode,
        linearNavigationIndex,
    } from '../../lib/collectionNavigation';
    import { compareNamedItems } from '../../lib/naturalSort';
    import { formatSequenceTempo } from '../../lib/sequenceTempo';
    import type { ObjectRenameTarget, PackageExportObject, SequenceItem } from '../../lib/types';
    import CollectionToolbar from '../../lib/components/CollectionToolbar.svelte';
    import ObjectContextMenu from '../../lib/components/ObjectContextMenu.svelte';

    interface Props {
        sequences: SequenceItem[];
        activeObjectId: string;
        query: string;
        onquerychange: (value: string) => void;
        onselect: (sequence: SequenceItem) => void;
        objectRenameAvailable?: boolean;
        onrenameobject?: (target: ObjectRenameTarget) => void;
        objectDeletionAvailable?: boolean;
        ondeleteobjects?: (objects: PackageExportObject[]) => void;
        packageExportAvailable?: boolean;
        onexportobjects?: (objects: PackageExportObject[]) => void;
        sequenceExportAvailable?: boolean;
        onexportmidi?: (objects: PackageExportObject[]) => void;
        sequenceImportAvailable?: boolean;
        onimportmidi?: () => void;
        selection?: PackageExportSelectionState;
        onselectionchange?: (selection: PackageExportSelectionState) => void;
        onselectionlimit?: () => void;
    }

    let {
        sequences,
        activeObjectId,
        query,
        onquerychange,
        onselect,
        objectRenameAvailable = false,
        onrenameobject = () => undefined,
        objectDeletionAvailable = false,
        ondeleteobjects = () => undefined,
        packageExportAvailable = false,
        onexportobjects = () => undefined,
        sequenceExportAvailable = false,
        onexportmidi = () => undefined,
        sequenceImportAvailable = false,
        onimportmidi = () => undefined,
        selection = emptyPackageExportSelection(),
        onselectionchange = () => undefined,
        onselectionlimit = () => undefined,
    }: Props = $props();

    let objectMenu = $state<{
        item: SequenceItem;
        objects: PackageExportObject[];
        left: number;
        top: number;
    } | null>(null);

    function exportObject(item: SequenceItem): PackageExportObject {
        return {
            kind: 'SEQU',
            objectId: item.objectId,
            name: item.name,
            typeLabel: 'Sequence',
            partitionIndex: item.object.partitionIndex,
            partitionName: item.object.partitionName,
            volumeName: item.object.volumeName,
        };
    }

    function domainObjects(): PackageExportObject[] {
        return orderedSequences.map(exportObject);
    }

    function visibleObjects(): PackageExportObject[] {
        return filteredSequences.map(exportObject);
    }

    function domainKey(): string {
        const first = sequences[0];
        return `sequences\u0000${first?.object.partitionIndex ?? ''}\u0000${first?.object.volumeName ?? ''}`;
    }

    function updateSelection(mode: ObjectSelectionMode, objectId: string): ObjectSelectionMode {
        const result = updatePackageExportSelection(
            selection,
            domainKey(),
            domainObjects(),
            visibleObjects(),
            objectId,
            mode,
        );
        if (result.limitExceeded) onselectionlimit();
        else onselectionchange(result.selection);
        return mode;
    }

    function selectAll(event: KeyboardEvent, objectId: string): boolean {
        if (!(event.ctrlKey || event.metaKey) || event.key.toLocaleLowerCase() !== 'a') return false;
        event.preventDefault();
        const result = updatePackageExportSelection(
            selection,
            domainKey(),
            domainObjects(),
            visibleObjects(),
            objectId,
            'all',
        );
        if (result.limitExceeded) onselectionlimit();
        else onselectionchange(result.selection);
        return true;
    }

    function openMenu(event: MouseEvent, item: SequenceItem): void {
        if (!objectRenameAvailable && !objectDeletionAvailable && !packageExportAvailable && !sequenceExportAvailable) {
            return;
        }
        event.preventDefault();
        let menuSelection = selection;
        if (!selection.items.some((selected) => selected.objectId === item.objectId)) {
            const result = updatePackageExportSelection(
                selection,
                domainKey(),
                domainObjects(),
                visibleObjects(),
                item.objectId,
                'replace',
            );
            menuSelection = result.selection;
            onselectionchange(menuSelection);
        }
        objectMenu = {
            item,
            objects: menuSelection.items,
            left: Math.max(8, Math.min(event.clientX, window.innerWidth - 180)),
            top: Math.max(8, Math.min(event.clientY, window.innerHeight - 56)),
        };
    }

    function openMenuFromKeyboard(event: KeyboardEvent, item: SequenceItem): void {
        if (selectAll(event, item.objectId)) return;
        if (event.key !== 'ContextMenu' && !(event.shiftKey && event.key === 'F10')) return;
        event.preventDefault();
        const bounds = (event.currentTarget as HTMLElement).getBoundingClientRect();
        openMenu(
            new MouseEvent('contextmenu', {
                clientX: bounds.left + Math.min(24, bounds.width / 2),
                clientY: bounds.top + Math.min(24, bounds.height / 2),
            }),
            item,
        );
    }

    function handleSequenceKeyboard(event: KeyboardEvent, currentIndex: number, item: SequenceItem): void {
        if (!hasDisallowedNavigationModifier(event)) {
            const targetIndex = linearNavigationIndex(
                event.key,
                currentIndex,
                filteredSequences.length,
                collectionPageStep(event.currentTarget),
            );
            if (targetIndex !== null) {
                event.preventDefault();
                if (targetIndex === currentIndex) return;
                const target = filteredSequences[targetIndex];
                if (!target) return;
                const mode = keyboardSelectionMode(event);
                updateSelection(mode, target.objectId);
                if (mode === 'replace') onselect(target);
                void focusCollectionIndex(event.currentTarget, targetIndex);
                return;
            }
        }
        openMenuFromKeyboard(event, item);
    }

    function clearSelection(event: MouseEvent): void {
        if (
            event.button !== 0 ||
            event.ctrlKey ||
            event.metaKey ||
            event.shiftKey ||
            selection.items.length === 0 ||
            (event.target instanceof Element && event.target.closest('.sequence-row'))
        ) {
            return;
        }
        onselectionchange(emptyPackageExportSelection());
    }

    function metadata(item: SequenceItem): string {
        const sequence = item.object.sequence;
        if (!sequence) return 'Sequence metadata unavailable';
        const tempo = ` · ${formatSequenceTempo(sequence.effectiveInitialTempoMicrosecondsPerQuarterNote)}`;
        return `${sequence.eventCount.toLocaleString()} events · ${sequence.ticksPerQuarterNote} PPQN${tempo}`;
    }

    const orderedSequences = $derived(sequences.toSorted(compareNamedItems));
    const normalizedQuery = $derived(query.trim().toLocaleLowerCase());
    const filteredSequences = $derived(
        normalizedQuery
            ? orderedSequences.filter((item) => item.name.toLocaleLowerCase().includes(normalizedQuery))
            : orderedSequences,
    );
</script>

<section class="collection-panel" aria-label="Sequences">
    <CollectionToolbar
        title="Sequences"
        count={sequences.length}
        {query}
        {onquerychange}
        actionLabel={sequenceImportAvailable ? 'Import MIDI' : undefined}
        onaction={onimportmidi}
    />
    <!-- Blank-space clearing is a pointer shortcut; the selection toolbar exposes the keyboard-accessible command. -->
    <!-- svelte-ignore a11y_click_events_have_key_events -->
    <!-- svelte-ignore a11y_no_static_element_interactions -->
    <div
        class:empty-collection={filteredSequences.length === 0}
        class="collection-body sequence-list"
        data-collection-list="sequences"
        data-navigation-list
        onclick={clearSelection}
    >
        {#each filteredSequences as item, index (item.id)}
            <button
                type="button"
                class:active={activeObjectId === item.objectId}
                class:selected={selection.items.some((selected) => selected.objectId === item.objectId)}
                class="sequence-row"
                data-collection-object-id={item.objectId}
                data-navigation-index={index}
                aria-pressed={selection.items.some((selected) => selected.objectId === item.objectId)}
                onclick={(event) => {
                    if (updateSelection(selectionMode(event), item.objectId) === 'replace') onselect(item);
                }}
                oncontextmenu={(event) => openMenu(event, item)}
                onkeydown={(event) => handleSequenceKeyboard(event, index, item)}
            >
                <strong>{item.name}</strong>
                <span>{metadata(item)}</span>
            </button>
        {:else}
            <p class="empty-copy">No matching Sequences</p>
        {/each}
    </div>
</section>

{#if objectMenu}
    <ObjectContextMenu
        objectName={objectMenu.objects.length === 1
            ? objectMenu.objects[0]!.name
            : `${objectMenu.objects.length} objects`}
        selectionCount={objectMenu.objects.length}
        left={objectMenu.left}
        top={objectMenu.top}
        onclose={() => (objectMenu = null)}
        onrename={objectRenameAvailable && objectMenu.objects.length === 1
            ? () =>
                  onrenameobject({
                      kind: 'sequence',
                      object: objectMenu!.item.object,
                      name: objectMenu!.item.name,
                  })
            : undefined}
        onexportpackage={packageExportAvailable ? () => onexportobjects(objectMenu!.objects) : undefined}
        onexportmidi={sequenceExportAvailable ? () => onexportmidi(objectMenu!.objects) : undefined}
        ondelete={objectDeletionAvailable ? () => ondeleteobjects(objectMenu!.objects) : undefined}
    />
{/if}
