<script lang="ts">
    import type { ConversionReport } from '../conversions';
    import Icon from './Icon.svelte';
    let {
        report,
        acknowledged = $bindable<string[]>([]),
        disabled = false,
    }: {
        report: ConversionReport;
        acknowledged?: string[];
        disabled?: boolean;
    } = $props();
    let page = $state(0);
    const pageSize = 100;
    const pageCount = $derived(
        Math.max(1, Math.ceil(Math.max(report.actions.length, report.issues.length) / pageSize)),
    );
    const currentPage = $derived(Math.min(page, pageCount - 1));
    function toggle(id: string, checked: boolean) {
        acknowledged = checked ? [...acknowledged, id] : acknowledged.filter((item) => item !== id);
    }
</script>

<section class="conversion-report" aria-label="Conversion report">
    <header>
        <strong>{report.actionCount} objects</strong><span>{report.issueCount} mapping details</span>
        <nav aria-label="Report pages">
            <button
                class="icon-button previous-page"
                aria-label="Previous report page"
                disabled={currentPage === 0}
                onclick={() => (page = currentPage - 1)}><Icon name="chevron" size={14} /></button
            >
            <span>{currentPage + 1} / {pageCount}</span>
            <button
                class="icon-button"
                aria-label="Next report page"
                disabled={currentPage + 1 === pageCount}
                onclick={() => (page = currentPage + 1)}><Icon name="chevron" size={14} /></button
            >
        </nav>
    </header>
    <div class="report-scroll">
        <table aria-label="Planned objects">
            <thead><tr><th>Object</th><th>Type</th><th>Action</th></tr></thead>
            <tbody
                >{#each report.actions.slice(currentPage * pageSize, (currentPage + 1) * pageSize) as action, index (index)}
                    <tr
                        ><td
                            >{action.name}{#each action.details as detail, detailIndex (detailIndex)}<small
                                    >{detail.label}: {detail.value}</small
                                >{/each}</td
                        ><td>{action.kind}</td><td>{action.action}</td></tr
                    >
                {/each}</tbody
            >
        </table>
        <ul aria-label="Mapping details">
            {#each report.issues.slice(currentPage * pageSize, (currentPage + 1) * pageSize) as issue (issue.id)}
                <li class:blocker={issue.impact === 'BLOCKER'}>
                    {#if ['LOSS', 'UNCERTAINTY'].includes(issue.impact)}
                        <label
                            ><input
                                type="checkbox"
                                class="dialog-checkbox"
                                {disabled}
                                checked={acknowledged.includes(issue.id)}
                                onchange={(event) => toggle(issue.id, event.currentTarget.checked)}
                            />
                            <span
                                ><strong>{issue.impact}</strong>
                                {issue.message}<small>{issue.sourceObject} {issue.sourceParameter}</small></span
                            ></label
                        >
                    {:else}<span
                            ><strong>{issue.impact}</strong>
                            {issue.message}<small>{issue.sourceObject} {issue.sourceParameter}</small></span
                        >{/if}
                </li>
            {/each}
        </ul>
    </div>
</section>

<style>
    .conversion-report {
        display: flex;
        flex: 1;
        min-width: 0;
        min-height: 0;
        flex-direction: column;
        font-size: var(--dialog-body-font-size);
    }
    header,
    nav {
        display: flex;
        align-items: center;
        gap: 10px;
        flex-wrap: wrap;
    }
    header {
        min-height: 32px;
        border-bottom: 1px solid var(--color-border);
    }
    nav {
        margin-left: auto;
    }
    .previous-page :global(svg) {
        transform: rotate(180deg);
    }
    .report-scroll {
        overflow: auto;
        min-height: 0;
        flex: 1;
    }
    table {
        border-collapse: collapse;
        width: 100%;
        table-layout: fixed;
    }
    th,
    td {
        padding: 5px;
        text-align: left;
        vertical-align: top;
        overflow-wrap: anywhere;
        border-bottom: 1px solid var(--color-border);
    }
    th {
        position: sticky;
        top: 0;
        background: var(--color-panel);
        font-size: var(--dialog-label-font-size);
    }
    ul {
        list-style: none;
        padding: 0;
        margin: 8px 0;
    }
    li {
        padding: 6px 0;
        border-bottom: 1px solid var(--color-border);
        overflow-wrap: anywhere;
    }
    label {
        display: flex;
        align-items: flex-start;
        gap: 8px;
    }
    small {
        display: block;
        color: var(--color-text-muted);
        font-size: var(--dialog-metadata-font-size);
    }
    .blocker {
        color: var(--color-danger);
    }
</style>
