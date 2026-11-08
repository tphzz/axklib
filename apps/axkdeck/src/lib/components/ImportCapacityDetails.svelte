<script lang="ts">
    import type { ImportCapacityReview } from '../../features/import/importCapacityReview.svelte';
    import type { CapacityAdmission } from '../importCapacity';
    import { formatStoredSize } from '../formatBytes';
    let {
        review,
        admission = null,
        pending = false,
    }: {
        review: ImportCapacityReview;
        admission?: CapacityAdmission | null;
        pending?: boolean;
    } = $props();
    const current = $derived(pending ? null : admission?.target === review.target ? admission : review.admission);
</script>

<section class="capacity-details" aria-label="Sampler capacity">
    <header>
        <h4>Sampler capacity</h4>
        <small
            >{pending || review.busy
                ? 'Pending check'
                : current
                  ? current.allowed
                      ? 'Fits'
                      : 'Does not fit'
                  : 'Not checked'}</small
        >
    </header>
    <dl>
        <div>
            <dt>Load target</dt>
            <dd>{review.target === 'A3000' ? 'A3K' : 'A4K/A5K'}</dd>
        </div>
    </dl>
    {#each current?.reports ?? [] as report}
        {@const profile = report.profiles.find((profile) => profile.target === review.target)}
        {#if profile}
            <dl>
                {#if (current?.reports.length ?? 0) > 1}<div>
                        <dt>Volume</dt>
                        <dd>{report.volumeName}</dd>
                    </div>{/if}
                <div>
                    <dt>{profile.peakBytes === null ? 'Minimum parameter memory' : 'Peak parameter memory'}</dt>
                    <dd class:fits={profile.status === 'FITS'} class:blocked={profile.status !== 'FITS'}>
                        {profile.peakBytes !== null
                            ? formatStoredSize(profile.peakBytes)
                            : profile.minimumResidentBytes !== null
                              ? `At least ${formatStoredSize(profile.minimumResidentBytes)}`
                              : 'Unavailable'} / {formatStoredSize(profile.parameterByteLimit)}
                    </dd>
                </div>
                <div>
                    <dt>Shared slots</dt>
                    <dd>{profile.peakSlots ?? 'Unavailable'} / {profile.sharedObjectSlotLimit}</dd>
                </div>
            </dl>
            {#each profile.reasons as reason}<p class="blocked">{reason.message}</p>{/each}
        {/if}
    {/each}
    <p class="capacity-context">
        Fresh power-on, Wipe, then full Volume Load. Parameter memory is separate from audio RAM and disk space.
    </p>
</section>

<style>
    .capacity-details {
        margin: 0 0 4px;
    }
    header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        min-height: 28px;
        gap: 8px;
    }
    h4 {
        margin: 0;
        font-size: var(--dialog-section-font-size);
        font-weight: 600;
        color: var(--color-text-strong);
    }
    dl {
        margin: 0;
    }
    dl > div {
        display: flex;
        justify-content: space-between;
        align-items: start;
        gap: 8px;
        min-height: 24px;
    }
    dt,
    small,
    .capacity-context {
        color: var(--color-text-muted);
        font-size: var(--dialog-metadata-font-size);
    }
    dd {
        margin: 0;
        text-align: right;
        font-size: var(--dialog-body-font-size);
        font-variant-numeric: tabular-nums;
    }
    p {
        margin: 2px 0 4px;
    }
    .fits {
        color: var(--color-success);
    }
    .blocked {
        color: var(--color-danger);
    }
</style>
