<script lang="ts">
    import { tick, type Snippet } from 'svelte';
    import type { ConversionReport } from '../conversions';
    import ImportReviewLayout from './ImportReviewLayout.svelte';
    import ImportReportPagination from './ImportReportPagination.svelte';
    let {
        report,
        acknowledged = $bindable<string[]>([]),
        disabled = false,
        capacity,
        feedback,
        feedbackNotice = '',
        active = $bindable<'contents' | 'review'>('contents'),
    }: {
        report: ConversionReport;
        acknowledged?: string[];
        disabled?: boolean;
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
    const currentObjectPage = $derived(
        Math.min(objectPage, Math.max(0, Math.ceil(report.actions.length / pageSize) - 1)),
    );
    const currentIssuePage = $derived(Math.min(issuePage, Math.max(0, Math.ceil(report.issues.length / pageSize) - 1)));
    const required = $derived(report.issues.filter((issue) => ['LOSS', 'UNCERTAINTY'].includes(issue.impact)));
    const blockers = $derived(report.issues.filter((issue) => issue.impact === 'BLOCKER'));
    const information = $derived(
        report.issues.filter((issue) => !['LOSS', 'UNCERTAINTY', 'BLOCKER'].includes(issue.impact)),
    );
    const orderedIssues = $derived([...blockers, ...required, ...information]);
    const visibleIssues = $derived(orderedIssues.slice(currentIssuePage * pageSize, (currentIssuePage + 1) * pageSize));
    const remaining = $derived(required.filter((issue) => !acknowledged.includes(issue.id)).length);
    const strict = $derived(report.fidelity === 'STRICT');
    const reviewNotice = $derived(
        blockers.length
            ? `${blockers.length} ${blockers.length === 1 ? 'blocker' : 'blockers'}`
            : remaining
              ? `${remaining} to acknowledge`
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
            <li
                class:blocker={issue.impact === 'BLOCKER'}
                class:required={['LOSS', 'UNCERTAINTY'].includes(issue.impact)}
            >
                {#if ['LOSS', 'UNCERTAINTY'].includes(issue.impact)}
                    <label>
                        <input
                            type="checkbox"
                            class="dialog-checkbox"
                            disabled={disabled || strict}
                            checked={acknowledged.includes(issue.id)}
                            onchange={(event) => toggle(issue.id, event.currentTarget.checked)}
                        />
                        <span
                            ><strong>{issue.impact === 'LOSS' ? 'Loss' : 'Uncertainty'}</strong> · {issue.message}</span
                        >
                    </label>
                {:else}<p>
                        <strong>{issue.impact === 'BLOCKER' ? 'Blocked' : 'Information'}</strong> · {issue.message}
                    </p>{/if}
                <small
                    >Source: {issue.sourceObject || 'Disk set'}{issue.sourceParameter
                        ? ` · ${issue.sourceParameter}`
                        : ''}</small
                >
            </li>
        {/each}
    </ul>
{/snippet}

<ImportReviewLayout
    bind:active
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
            {#if blockers.length}<h4 class="blocker">
                    {blockers.length} blocking {blockers.length === 1 ? 'issue' : 'issues'}
                </h4>
                {@render issueRows(visibleIssues.filter((issue) => issue.impact === 'BLOCKER'))}{/if}
            {#if strict && required.length}<p class="blocker">
                    Strict fidelity does not allow loss or uncertainty.
                </p>{/if}
            {@render capacity?.()}
            {#if required.length}
                <div class="review-heading">
                    <h4>Required acknowledgements</h4>
                    <small>{required.length - remaining} of {required.length} accepted</small>
                </div>
                {@render issueRows(visibleIssues.filter((issue) => ['LOSS', 'UNCERTAINTY'].includes(issue.impact)))}
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
            {#if !report.issues.length}<p class="empty">No mapping issues</p>{/if}
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
