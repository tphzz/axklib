<script lang="ts">
    import { untrack } from 'svelte';
    import { useASeriesPreferences, type ASeriesGeneration } from '../aSeriesPreferences.svelte';
    import type { ImportCapacityReview } from '../../features/import/importCapacityReview.svelte';
    import type { CapacityAdmission } from '../importCapacity';
    import ImportCapacitySummary from './ImportCapacitySummary.svelte';

    let {
        review,
        admission = null,
        disabled = false,
        onchange = () => undefined,
        compact = false,
        showSummary = true,
    }: {
        review: ImportCapacityReview;
        admission?: CapacityAdmission | null;
        disabled?: boolean;
        onchange?: () => void;
        compact?: boolean;
        showSummary?: boolean;
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
    function select(target: ASeriesGeneration): void {
        if (disabled || review.busy || target === review.target) return;
        review.target = target;
        review.reset();
        onchange();
    }
</script>

<div class="import-capacity" class:compact>
    <div class="capacity-control">
        <span>Sampler load target</span>
        <div
            class="dialog-segmented-control"
            class:dialog-segmented-control--compact={compact}
            role="group"
            aria-label="Sampler load target"
        >
            {#each ['A3000', 'A4000_A5000'] as target}
                <button
                    type="button"
                    disabled={disabled || review.busy}
                    aria-pressed={review.target === target}
                    onclick={() => select(target as ASeriesGeneration)}
                >
                    {target === 'A3000' ? (compact ? 'A3K' : 'a3k') : compact ? 'A4K/A5K' : 'a4k/a5k'}
                </button>
            {/each}
        </div>
    </div>
    {#if showSummary}<ImportCapacitySummary {review} {admission} {compact} />{/if}
</div>

<style>
    .import-capacity {
        min-width: 0;
    }
    .capacity-control {
        display: flex;
        flex-wrap: wrap;
        align-items: center;
        justify-content: space-between;
        gap: 12px;
        padding: 8px 0;
    }
    .capacity-control > span {
        color: var(--color-text-muted);
        font-size: var(--dialog-label-font-size);
    }
    .compact .capacity-control {
        flex-wrap: nowrap;
        justify-content: flex-start;
        gap: 4px;
        padding: 0;
    }
    .compact .capacity-control > span {
        width: 104px;
        flex: none;
    }
    @container audio-import (max-width: 365px) {
        .compact .capacity-control {
            flex-wrap: wrap;
        }
    }
</style>
