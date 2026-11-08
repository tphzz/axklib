<script lang="ts">
    import type { ImportCapacityReview } from '../../features/import/importCapacityReview.svelte';
    import { formatStoredSize } from '../formatBytes';
    import type { CapacityAdmission } from '../importCapacity';

    let {
        review,
        admission = null,
        compact = false,
    }: {
        review: ImportCapacityReview;
        admission?: CapacityAdmission | null;
        compact?: boolean;
    } = $props();
    const current = $derived(admission?.target === review.target ? admission : review.admission);
    const summary = $derived(
        current?.reports
            .map((report) => {
                const profile = report.profiles.find((profile) => profile.target === review.target);
                if (!profile) return '';
                return `${report.volumeName}: ${profile.status === 'FITS' ? 'Fits' : 'Does not fit'} · ${profile.peakBytes !== null ? `${formatStoredSize(profile.peakBytes)} peak` : profile.minimumResidentBytes !== null ? `at least ${formatStoredSize(profile.minimumResidentBytes)}` : 'parameter memory unavailable'} / ${formatStoredSize(profile.parameterByteLimit)} · ${profile.peakSlots === null ? 'shared slots unavailable' : `${profile.peakSlots} / ${profile.sharedObjectSlotLimit} shared slots`}${profile.reasons.length ? `. ${profile.reasons.map((reason) => reason.message).join(' ')}` : ''}`;
            })
            .filter(Boolean)
            .join('\n') ??
            'Fresh power-on, Wipe, then full Volume Load. Parameter memory is separate from audio RAM and disk space.',
    );
</script>

{#if compact}
    <!-- svelte-ignore a11y_no_noninteractive_tabindex (The bounded capacity report must support keyboard scrolling.) -->
    <div
        class="capacity-summary compact"
        aria-live="polite"
        role="region"
        aria-label="Sampler load capacity"
        tabindex="0"
        title={summary}
    >
        {review.busy ? review.message : summary}
    </div>
{:else}
    <div class="capacity-summary" aria-live="polite" title={summary}>{review.busy ? review.message : summary}</div>
{/if}

<style>
    .capacity-summary {
        height: 34px;
        min-width: 0;
        overflow: auto;
        white-space: pre-line;
        overflow-wrap: anywhere;
        font-size: var(--dialog-metadata-font-size);
        color: var(--color-text-muted);
    }
    .capacity-summary.compact {
        height: auto;
        max-height: 42px;
        line-height: 14px;
    }
    .capacity-summary:focus-visible {
        outline: 1px solid var(--state-focus);
        outline-offset: -1px;
    }
</style>
