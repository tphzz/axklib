<script lang="ts">
    import { tick, type Snippet } from 'svelte';
    import type { ConversionReport } from '../conversions';
    import ImportReviewLayout from './ImportReviewLayout.svelte';
    import ImportReportPagination from './ImportReportPagination.svelte';
    let {
        report: prepared,
        acknowledged = $bindable<string[]>([]),
        disabled = false,
        sourceSelection,
        sourceSummary = '',
        capacity,
        feedback,
        feedbackNotice = '',
        active = $bindable<'contents' | 'review'>('contents'),
    }: {
        report: ConversionReport | null;
        acknowledged?: string[];
        disabled?: boolean;
        sourceSelection?: Snippet;
        sourceSummary?: string;
        capacity?: Snippet;
        feedback?: Snippet;
        feedbackNotice?: string;
        active?: 'contents' | 'review';
    } = $props();
    let objectPage = $state(0);
    let issuePage = $state(0);
    let objectList: HTMLDivElement;
    let issueList: HTMLDivElement;
    const pageSize = 100;
    const report = $derived(prepared ?? { actions: [], issues: [], actionCount: 0, issueCount: 0, fidelity: 'BEST' });
    const currentObjectPage = $derived(
        Math.min(objectPage, Math.max(0, Math.ceil(report.actions.length / pageSize) - 1)),
    );
    const currentIssuePage = $derived(Math.min(issuePage, Math.max(0, Math.ceil(report.issues.length / pageSize) - 1)));
    const required = $derived(report.issues.filter((issue) => !!issue.decision));
    const notes = $derived(
        report.issues.filter((issue) => !issue.decision && ['LOSS', 'UNCERTAINTY'].includes(issue.impact)),
    );
    const blockers = $derived(report.issues.filter((issue) => issue.impact === 'BLOCKER'));
    const information = $derived(
        report.issues.filter((issue) => !['LOSS', 'UNCERTAINTY', 'BLOCKER'].includes(issue.impact)),
    );
    const orderedIssues = $derived([...blockers, ...required, ...notes, ...information]);
    const visibleIssues = $derived(orderedIssues.slice(currentIssuePage * pageSize, (currentIssuePage + 1) * pageSize));
    const remaining = $derived(required.filter((issue) => !acknowledged.includes(issue.id)).length);
    const strict = $derived(report.fidelity === 'STRICT');
    const sources = $derived(new Map((prepared?.sourceObjects ?? []).map((object) => [object.id, object])));
    function sourceLabel(id: string): string {
        const source = sources.get(id);
        if (source) return `${source.name} · ${source.location}`;
        return id && !id.startsWith('tx16w:') ? id : 'Disk set';
    }
    const reviewNotice = $derived(
        blockers.length
            ? `${blockers.length} ${blockers.length === 1 ? 'blocker' : 'blockers'}`
            : strict && (required.length || notes.length)
              ? 'Blocked by Strict'
              : remaining
                ? `${remaining} mapping decisions`
                : feedbackNotice,
    );
    async function changePage(pane: 'contents' | 'review', page: number) {
        if (pane === 'contents') objectPage = page;
        else issuePage = page;
        await tick();
        const scroller = (pane === 'contents' ? objectList : issueList)?.closest('.import-pane-scroll');
        if (scroller) scroller.scrollTop = 0;
    }
    function toggle(id: string, checked: boolean) {
        if (disabled || strict) return;
        acknowledged = checked ? [...new Set([...acknowledged, id])] : acknowledged.filter((item) => item !== id);
    }
    function kindLabel(kind: string): string {
        return (
            (
                {
                    'wave-data': 'Wave Data',
                    sample: 'Sample',
                    'sample-bank': 'Sample Bank',
                    program: 'Program',
                    sequence: 'Sequence',
                } as Record<string, string>
            )[kind] ?? kind
        );
    }
</script>

{#snippet issueRows(issues: ConversionReport['issues'])}
    <ul>
        {#each issues as issue (issue.id)}
            <li class:blocker={issue.impact === 'BLOCKER'} class:required={!!issue.decision}>
                {#if issue.decision && !strict}
                    <label>
                        <input
                            type="checkbox"
                            class="dialog-checkbox"
                            disabled={disabled || strict}
                            checked={acknowledged.includes(issue.id)}
                            onchange={(event) => toggle(issue.id, event.currentTarget.checked)}
                        />
                        <span>{issue.decision}</span>
                    </label>
                {:else}<p>
                        {#if issue.impact === 'BLOCKER'}<strong>Blocked</strong> ·
                        {/if}{issue.message}
                    </p>{/if}
                <small title={issue.sourceObject}
                    >Source: {sourceLabel(issue.sourceObject)}{issue.sourceParameter
                        ? ` · ${issue.sourceParameter}`
                        : ''}</small
                >
            </li>
        {/each}
    </ul>
{/snippet}

<ImportReviewLayout
    bind:active
    {sourceSelection}
    {sourceSummary}
    contentsSummary={`${report.actionCount} planned objects`}
    reviewSummary={`${report.issueCount} mapping details`}
    {reviewNotice}
>
    {#snippet columns()}<div class="object-columns">
            <span>Object</span><span>Type</span><span>Action</span>
        </div>{/snippet}
    {#snippet contents()}
        <div role="list" aria-label="Planned objects" bind:this={objectList}>
            {#each report.actions.slice(currentObjectPage * pageSize, (currentObjectPage + 1) * pageSize) as action, index (index)}
                <div class="object-row" role="listitem">
                    <strong>{action.name}</strong><span>{kindLabel(action.kind)}</span><span>{action.action}</span>
                    <small
                        >{#each action.details as detail, i}{i ? ' · ' : ''}{detail.label}: {detail.value}{/each}</small
                    >
                </div>
            {:else}<p class="empty">No planned objects</p>{/each}
        </div>
    {/snippet}
    {#snippet review()}
        <div bind:this={issueList}>
            {@render feedback?.()}
            {#if sourceSelection}<dl class="summary">
                    <div>
                        <dt>Source selection</dt>
                        <dd>{sourceSummary}</dd>
                    </div>
                    <div>
                        <dt>Planned output</dt>
                        <dd>{prepared ? `${report.actionCount} objects` : 'No current plan'}</dd>
                    </div>
                </dl>{/if}
            {#if blockers.length}<h4 class="blocker">
                    {blockers.length} blocking {blockers.length === 1 ? 'issue' : 'issues'}
                </h4>
                {@render issueRows(visibleIssues.filter((issue) => issue.impact === 'BLOCKER'))}{/if}
            {#if strict && (required.length || notes.length)}<p class="blocker">
                    Strict fidelity cannot preserve this mapping. Choose Best or change the source selection.
                </p>{/if}
            {@render capacity?.()}
            {#if required.length}
                <div class="review-heading">
                    <h4>Mapping decisions</h4>
                </div>
                {@render issueRows(visibleIssues.filter((issue) => !!issue.decision))}
            {/if}
            {#if notes.length}
                <h4>Conversion notes</h4>
                {@render issueRows(
                    visibleIssues.filter((issue) => !issue.decision && ['LOSS', 'UNCERTAINTY'].includes(issue.impact)),
                )}
            {/if}
            {#if information.length}
                <div class="review-heading">
                    <h4>Other mapping details</h4>
                    <small>{information.length}</small>
                </div>
                {@render issueRows(
                    visibleIssues.filter((issue) => !['LOSS', 'UNCERTAINTY', 'BLOCKER'].includes(issue.impact)),
                )}
            {/if}
            {#if !report.issues.length}<p class="empty">{prepared ? 'No mapping issues' : 'No current plan'}</p>{/if}
        </div>
    {/snippet}
    {#snippet contentsFooter()}<ImportReportPagination
            label="objects"
            count={report.actions.length}
            page={currentObjectPage}
            onchange={(page) => void changePage('contents', page)}
        />{/snippet}
    {#snippet reviewFooter()}<ImportReportPagination
            label="mapping details"
            count={report.issues.length}
            page={currentIssuePage}
            onchange={(page) => void changePage('review', page)}
        />{/snippet}
</ImportReviewLayout>

<style>
    .object-columns,
    .object-row {
        display: grid;
        grid-template-columns: minmax(0, 1fr) 110px 70px;
        column-gap: 4px;
        padding: 4px 6px;
    }
    .object-columns {
        min-height: 24px;
        align-items: center;
        font-size: var(--dialog-label-font-size);
    }
    .object-row {
        min-height: 38px;
        align-content: start;
        row-gap: 2px;
        font-size: var(--dialog-body-font-size);
    }
    .object-row strong,
    .object-row span {
        overflow-wrap: anywhere;
    }
    .object-row small {
        grid-column: 1 / -1;
    }
    small {
        font-size: var(--dialog-metadata-font-size);
        color: var(--color-text-muted);
        line-height: 14px;
    }
    h4 {
        margin: 0;
        font-size: var(--dialog-section-font-size);
        font-weight: 600;
        color: var(--color-text-strong);
    }
    .summary {
        padding: 4px 0;
        margin: 0;
        border-block: 1px solid var(--color-border);
    }
    .summary > div {
        display: flex;
        justify-content: space-between;
        gap: 8px;
        align-items: center;
        min-height: 25px;
    }
    .summary dt {
        color: var(--color-text-muted);
    }
    .summary dd {
        margin: 0;
        text-align: right;
    }
    .review-heading {
        display: flex;
        justify-content: space-between;
        align-items: center;
        gap: 8px;
        min-height: 28px;
    }
    ul {
        list-style: none;
        padding: 0;
        margin: 0;
    }
    li {
        padding: 6px 0;
        overflow-wrap: anywhere;
    }
    li small {
        display: block;
        margin-top: 4px;
    }
    label {
        display: flex;
        align-items: flex-start;
        gap: 8px;
    }
    p {
        margin: 4px 0;
    }
    .required label {
        color: var(--color-warning);
    }
    .blocker {
        color: var(--color-danger);
    }
    .empty {
        color: var(--color-text-muted);
        padding: 6px 0;
    }
    @media (max-width: 480px) {
        .object-columns,
        .object-row {
            grid-template-columns: minmax(0, 1fr) 72px 50px;
        }
    }
</style>
