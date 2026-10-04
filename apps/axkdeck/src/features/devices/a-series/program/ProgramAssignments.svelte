<script lang="ts">
    import { tick } from 'svelte';
    import { linearNavigationIndex } from '../../../../lib/collectionNavigation';
    import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';
    import type { ProgramEditingSnapshot, ProgramEditorFormat } from '../../../../lib/objectEditing';
    import type { InspectorSelection, ProgramSampleSelectRow } from '../../../../lib/types';
    import CollectionToolbar from '../../../../lib/components/CollectionToolbar.svelte';
    import EditorChoice from '../../../object-editor/EditorChoice.svelte';
    import { modal } from '../../../../lib/modal';
    import { ProgramDraft } from './draft.svelte';
    import { receiveOptions } from './fields';
    let {
        document,
        snapshot,
        format,
        disabled,
        query,
        onquerychange,
        selection,
        onselect,
    }: {
        document: ObjectEditorDocument;
        snapshot: ProgramEditingSnapshot;
        format: ProgramEditorFormat;
        disabled: boolean;
        query: string;
        onquerychange: (value: string) => void;
        selection: InspectorSelection;
        onselect: (row: ProgramSampleSelectRow) => void;
    } = $props();
    const draft = $derived(document.draft as ProgramDraft);
    let assignedOnly = $state(true);
    let pendingRemoval = $state<number | null>(null);
    let scroller: HTMLDivElement;
    let height = $state(300);
    let top = $state(0);
    let focusedKey = $state('');
    const rowHeight = 36;
    const targets = $derived(new Map(snapshot.targets.map((target) => [target.objectId, target])));
    const defaults = $derived(
        Object.fromEntries(
            format.fields
                .filter((field) => field.key.startsWith('assignments.*.') && field.defaultValue !== undefined)
                .map((field) => [field.key.slice('assignments.*.'.length), field.defaultValue!]),
        ),
    );
    const options = $derived([{ value: -2, label: 'Off' }, ...receiveOptions(snapshot.model === 'A3000')]);
    const rows = $derived.by(() => {
        const assigned = draft.assignments
            .filter((row) => row.name)
            .map((row) => ({
                key: `assigned-${row.id}`,
                assignmentId: row.id as number | undefined,
                ...row,
                target: row.targetObjectId ? targets.get(row.targetObjectId) : undefined,
            }));
        const assignedTargets = new Set(assigned.map((row) => row.targetObjectId));
        const available = assignedOnly
            ? []
            : snapshot.targets
                  .filter((target) => !assignedTargets.has(target.objectId))
                  .map((target) => ({
                      key: target.objectId,
                      assignmentId: undefined,
                      id: -1,
                      kind: target.kind,
                      name: target.name,
                      targetObjectId: target.objectId,
                      target,
                  }));
        return [...assigned, ...available];
    });
    const filtered = $derived(
        rows.filter((row) =>
            `${row.name} ${row.kind === 'SBAC' ? 'Sample Bank' : 'Sample'}`
                .toLocaleLowerCase()
                .includes(query.trim().toLocaleLowerCase()),
        ),
    );
    const indices = $derived.by(() => {
        const start = Math.max(0, Math.min(filtered.length - 1, Math.floor(top / rowHeight)) - 8);
        const end = Math.min(filtered.length, start + Math.ceil(height / rowHeight) + 17);
        const visible = Array.from({ length: Math.max(0, end - start) }, (_, index) => start + index);
        const focused = filtered.findIndex((row) => row.key === focusedKey);
        if (focused >= 0 && !visible.includes(focused)) visible.push(focused);
        return visible.sort((a, b) => a - b);
    });
    $effect(() => {
        query;
        assignedOnly;
        top = 0;
        if (scroller) scroller.scrollTop = 0;
    });
    function observe(node: HTMLDivElement) {
        const resize = new ResizeObserver(() => (height = node.clientHeight || 300));
        resize.observe(node);
        return { destroy: () => resize.disconnect() };
    }
    async function focusRow(index: number, column = 0) {
        const offset = index * rowHeight;
        if (offset < top || offset + rowHeight > top + height - 24) {
            top = Math.max(0, offset - (height - 24 - rowHeight) / 2);
            scroller.scrollTop = top;
        }
        focusedKey = filtered[index]?.key ?? '';
        await tick();
        scroller
            .querySelectorAll<HTMLButtonElement>(`[data-row-index="${index}"] button`)
            [column]?.focus({ preventScroll: true });
    }
    function navigate(event: KeyboardEvent, index: number, column: number) {
        const next =
            event.key === 'Tab'
                ? column === 1 && !event.shiftKey
                    ? index + 1
                    : column === 0 && event.shiftKey
                      ? index - 1
                      : null
                : column === 0
                  ? linearNavigationIndex(
                        event.key,
                        index,
                        filtered.length,
                        Math.max(1, Math.floor(height / rowHeight) - 1),
                    )
                  : null;
        if (next === null || next < 0 || next >= filtered.length || event.altKey || event.ctrlKey || event.metaKey)
            return;
        event.preventDefault();
        void focusRow(next, event.key === 'Tab' && event.shiftKey ? 1 : 0);
    }
    function select(row: (typeof rows)[number]) {
        if (row.assignmentId !== undefined) document.programAssignmentId = row.assignmentId;
        const saved =
            selection?.kind === 'program'
                ? selection.sampleSelect.all.find((item) => item.targetObjectId === row.targetObjectId)
                : undefined;
        if (saved) onselect(saved);
    }
    function change(row: (typeof rows)[number], value: number) {
        if (disabled) return;
        const id = row.assignmentId;
        if (value === -2) {
            if (id === undefined) return;
            const hasEdits = Object.entries(defaults).some(
                ([key, neutral]) =>
                    key !== 'receive' &&
                    draft.values[`assignments.${id}.${key}`] !== undefined &&
                    draft.values[`assignments.${id}.${key}`] !== neutral,
            );
            if (hasEdits) pendingRemoval = id;
            else draft.removeAssignment(id);
        } else if (id !== undefined) {
            draft.set(`assignments.${id}.receive`, value);
            document.programAssignmentId = id;
        } else if (row.target?.assignable && draft.assignments.length < 999) {
            document.programAssignmentId = draft.addAssignment(row.target, { ...defaults, receive: value });
        }
    }
</script>

<CollectionToolbar
    title="Assignments"
    count={rows.length}
    {query}
    {onquerychange}
    filterLabel="Show only assigned"
    filterChecked={assignedOnly}
    onfilterchange={(value) => (assignedOnly = value)}
/>
<div class="assignment-scroll" bind:this={scroller} use:observe onscroll={() => (top = scroller.scrollTop)}>
    <table aria-label="Program assignments" aria-rowcount={filtered.length + 1}>
        <thead><tr><th>Sample/Bank</th><th>Receive channel assign</th></tr></thead>
        <tbody>
            {#each indices as index, position (filtered[index]!.key)}
                {@const row = filtered[index]!}
                {@const gap = index - (indices[position - 1] ?? -1) - 1}
                {#if gap > 0}<tr aria-hidden="true" class="spacer" style:height={`${gap * rowHeight}px`}
                        ><td colspan="2"></td></tr
                    >{/if}
                <tr
                    data-row-index={index}
                    aria-rowindex={index + 2}
                    onfocusin={() => (focusedKey = row.key)}
                    class:selected={row.assignmentId !== undefined && document.programAssignmentId === row.assignmentId}
                >
                    <td
                        ><button
                            type="button"
                            class="target"
                            aria-label={`${row.name} ${row.kind === 'SBAC' ? 'Sample Bank' : 'Sample'}`}
                            disabled={disabled && row.assignmentId === undefined}
                            aria-pressed={row.assignmentId !== undefined &&
                                document.programAssignmentId === row.assignmentId}
                            onclick={() => select(row)}
                            onkeydown={(event) => navigate(event, index, 0)}
                        >
                            <strong>{row.name}</strong><small
                                >{row.kind === 'SBAC' ? 'Sample Bank' : 'Sample'}{row.target?.reason
                                    ? ` · ${row.target.reason}`
                                    : !row.targetObjectId
                                      ? ' · Unresolved reference'
                                      : ''}</small
                            >
                        </button></td
                    >
                    <td
                        ><EditorChoice
                            label={`Receive channel assign: ${row.name}${row.assignmentId !== undefined ? ` (${row.assignmentId + 1})` : ''}`}
                            value={row.assignmentId === undefined
                                ? -2
                                : Number(draft.values[`assignments.${row.assignmentId}.receive`])}
                            {options}
                            segmented={false}
                            ontriggerkeydown={(event) => navigate(event, index, 1)}
                            disabled={disabled ||
                                row.kind === 'UNKNOWN' ||
                                (row.assignmentId === undefined &&
                                    (!row.target?.assignable || draft.assignments.length >= 999))}
                            onchange={(value) => change(row, value)}
                        /></td
                    >
                </tr>
            {:else}<tr
                    ><td colspan="2" class="empty-copy"
                        >{query ? 'No matching Samples or Sample Banks' : 'No assigned Samples or Sample Banks'}</td
                    ></tr
                >{/each}
            {#if indices.length && indices.at(-1)! < filtered.length - 1}<tr
                    class="spacer"
                    aria-hidden="true"
                    style:height={`${(filtered.length - indices.at(-1)! - 1) * rowHeight}px`}><td colspan="2"></td></tr
                >{/if}
        </tbody>
    </table>
</div>
{#if pendingRemoval !== null}
    <div class="dialog-backdrop" role="presentation">
        <div
            class="dialog-shell remove-assignment"
            role="dialog"
            aria-modal="true"
            aria-labelledby="remove-assignment-title"
            use:modal={{ onescape: () => (pendingRemoval = null) }}
        >
            <header class="dialog-header"><h2 id="remove-assignment-title">Remove assignment?</h2></header>
            <p>
                Removing {draft.assignments.find((row) => row.id === pendingRemoval)?.name} also removes its Easy Edit settings
                from this Program.
            </p>
            <footer class="dialog-footer">
                <span class="dialog-footer-status"></span>
                <div class="dialog-footer-actions">
                    <button class="secondary-button" onclick={() => (pendingRemoval = null)}>Cancel</button>
                    <button
                        class="danger-button"
                        {disabled}
                        onclick={() => {
                            if (pendingRemoval !== null) draft.removeAssignment(pendingRemoval);
                            pendingRemoval = null;
                        }}>Remove</button
                    >
                </div>
            </footer>
        </div>
    </div>
{/if}

<style>
    .assignment-scroll {
        flex: 1;
        min-height: 0;
        overflow: auto;
        padding: 0 6px;
    }
    table {
        border-collapse: collapse;
        width: 100%;
        table-layout: fixed;
        font-size: 11px;
    }
    th {
        position: sticky;
        top: 0;
        z-index: 1;
        background: var(--color-panel);
        color: var(--color-text-muted);
        font-size: 10px;
        font-weight: 400;
        text-align: left;
        height: 24px;
    }
    th:last-child {
        width: 32%;
        min-width: 150px;
    }
    td {
        border-bottom: 1px solid var(--color-border);
        padding: 2px 6px;
    }
    tr[data-row-index] {
        height: 36px;
    }
    .spacer td {
        padding: 0;
        border: 0;
    }
    tr.selected {
        background: var(--color-panel-raised);
    }
    .target {
        display: block;
        width: 100%;
        text-align: left;
        border: 0;
        background: transparent;
        padding: 2px 0;
        color: var(--color-text);
        font: inherit;
        height: 30px;
        line-height: 13px;
    }
    strong,
    small {
        display: block;
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
    }
    small {
        color: var(--color-text-muted);
        font-size: 10px;
    }
    .target:focus-visible {
        outline: 1px solid var(--color-accent);
    }
    .remove-assignment {
        width: min(460px, calc(100vw - 32px));
    }
    .remove-assignment p {
        padding: 12px;
    }
</style>
