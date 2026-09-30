<script lang="ts">
    import { untrack } from 'svelte';
    import { useASeriesPreferences, type ASeriesGeneration } from '../aSeriesPreferences.svelte';
    import type { ImportCapacityReview } from '../../features/import/importCapacityReview.svelte';
    import { formatStoredSize } from '../formatBytes';
    import type { CapacityAdmission } from '../importCapacity';

    let {
        review,
        admission = null,
        disabled = false,
        onchange = () => undefined,
    }: {
        review: ImportCapacityReview;
        admission?: CapacityAdmission | null;
        disabled?: boolean;
        onchange?: () => void;
    } = $props();
    const preferences = useASeriesPreferences();
    let initialized = $state(false);
    $effect(() => {
        if (initialized || disabled || review.busy) return;
        untrack(() => {
            initialized = true;
            if (review.target !== preferences.generation) {
                review.target = preferences.generation;
                review.reset();
                onchange();
            }
        });
    });
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
    function select(target: ASeriesGeneration): void {
        if (disabled || review.busy || target === review.target) return;
        review.target = target;
        review.reset();
        onchange();
    }
</script>

<div class="capacity-control">
    <span>Sampler load target</span>
    <div class="dialog-segmented-control" role="group" aria-label="Sampler load target">
        {#each ['A3000', 'A4000_A5000'] as target}
            <button
                type="button"
                disabled={disabled || review.busy}
                aria-pressed={review.target === target}
                onclick={() => select(target as ASeriesGeneration)}
            >
                {target === 'A3000' ? 'a3k' : 'a4k/a5k'}
            </button>
        {/each}
    </div>
</div>
<div class="capacity-summary" aria-live="polite" title={summary}>{review.busy ? review.message : summary}</div>

<style>
    .capacity-control {
        display: flex;
        flex-wrap: wrap;
        align-items: center;
        justify-content: space-between;
        gap: 12px;
        padding: 8px 0;
    }
    .capacity-control > span {
        color: var(--text-muted);
        font-size: var(--dialog-label-font-size);
    }
    .capacity-summary {
        height: 34px;
        overflow: auto;
        white-space: pre-line;
        overflow-wrap: anywhere;
        font-size: var(--dialog-metadata-font-size);
        color: var(--text-muted);
    }
</style>
