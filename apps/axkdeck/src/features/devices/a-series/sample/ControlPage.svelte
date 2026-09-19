<script lang="ts">
    import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';
    import type { SamplePage } from './fields';
    import ParameterField from './ParameterField.svelte';
    import { parameterBlockReason } from './parameterAvailability';
    let { document, page, disabled }: { document: ObjectEditorDocument; page: SamplePage; disabled: boolean } =
        $props();
    const blocked = $derived(document.detail?.editing?.blockedParameters ?? []);
</script>

<table aria-label="Sample MIDI controls">
    <thead
        ><tr
            ><th scope="col">#</th>{#each ['Controller', 'Function', 'Type', 'Range'] as label}<th scope="col"
                    >{label}</th
                >{/each}</tr
        ></thead
    >
    <tbody>
        {#each [1, 2, 3, 4, 5, 6] as row}
            <tr>
                <th scope="row">{row}</th>
                {#each page.fields.filter((field) => field.key.startsWith(`controls.${row}.`)) as field (field.key)}
                    <td
                        ><ParameterField
                            field={{ ...field, label: `Control ${row} ${field.label}` }}
                            draft={document.draft}
                            unavailableReason={document.detail?.editing?.unavailableParameters[field.key]?.message}
                            disabled={disabled || blocked.includes(field.key)}
                            blockedReason={parameterBlockReason(field.key, blocked)}
                            oninvalid={(message) =>
                                (document.inputErrors = { ...document.inputErrors, [field.key]: message })}
                        /></td
                    >
                {/each}
            </tr>
        {/each}
    </tbody>
</table>

<style>
    table {
        width: 100%;
        table-layout: fixed;
        border-collapse: collapse;
        font-size: 11px;
    }
    thead {
        position: sticky;
        top: -8px;
        z-index: 2;
        background: var(--color-panel);
    }
    th {
        text-align: left;
        color: var(--color-text-muted);
        font-weight: 500;
    }
    th:first-child {
        width: 28px;
    }
    th:nth-child(3) {
        width: 29%;
    }
    th:last-child {
        width: 22%;
    }
    th,
    td {
        padding: var(--density-cell-padding, 3px) var(--density-panel-padding, 6px);
        border-bottom: 1px solid var(--color-border);
    }
    td {
        min-width: 0;
    }
    td :global(.parameter-field) {
        grid-template-columns: minmax(0, 1fr);
        gap: 0;
    }
    td :global(.field-label) {
        display: none;
    }
    :global([data-editor-under~='700']) thead {
        display: none;
    }
    :global([data-editor-under~='700']) tr {
        display: grid;
        grid-template-columns: 28px repeat(2, minmax(0, 1fr));
        border-bottom: 1px solid var(--color-border);
    }
    :global([data-editor-under~='700']) tbody th {
        grid-row: span 2;
        width: auto;
    }
    :global([data-editor-under~='700']) td {
        border: 0;
    }
    :global([data-editor-under~='700']) td :global(.field-label) {
        display: block;
        margin-bottom: 4px;
    }
    :global([data-editor-under~='420']) tr {
        grid-template-columns: 24px minmax(0, 1fr);
    }
    :global([data-editor-under~='420']) tbody th {
        grid-row: span 4;
    }
</style>
