<script lang="ts">
    import { onMount } from 'svelte';
    import { serverFileLocation, type DirectoryLocation, type FileLocation } from '../storageLocations';
    import type { HardDiskCreationProfile, HardDiskCreationProfileId, ImageTransport, JobState } from '../transport';
    import { userFacingMessage } from '../userFacingMessage';
    import Icon from './Icon.svelte';

    interface Props {
        transport: ImageTransport;
        directory: DirectoryLocation;
        onsuccess: (file: FileLocation) => void;
        oncancel: () => void;
    }

    let { transport, directory, onsuccess, oncancel }: Props = $props();
    let profiles = $state<HardDiskCreationProfile[]>([]);
    let profileId = $state<HardDiskCreationProfileId>('FLOPPY_SCALE');
    let partitionCount = $state(1);
    let fileName = $state('New disk');
    let loading = $state(true);
    let busy = $state(false);
    let activeJob = $state<JobState | null>(null);
    let error = $state('');
    const partitionCounts = [1, 2, 3, 4, 5, 6, 7, 8] as const;

    const selectedProfile = $derived(profiles.find((profile) => profile.profileId === profileId) ?? null);
    const selectedOption = $derived(
        selectedProfile?.partitionOptions.find((option) => option.partitionCount === partitionCount) ?? null,
    );

    onMount(async () => {
        try {
            profiles = await transport.hardDiskCreationProfiles();
            const initial = profiles[0];
            if (!initial) throw new Error('The server did not provide a hard-disk creation profile');
            selectProfile(initial.profileId);
        } catch (reason) {
            error = userFacingMessage(reason);
        } finally {
            loading = false;
        }
    });

    function selectProfile(nextProfileId: HardDiskCreationProfileId): void {
        const profile = profiles.find((candidate) => candidate.profileId === nextProfileId);
        if (!profile) return;
        profileId = nextProfileId;
        if (!profile.partitionOptions.some((option) => option.partitionCount === partitionCount)) {
            partitionCount = profile.defaultPartitionCount;
        }
    }

    function profileLabel(id: HardDiskCreationProfileId): string {
        switch (id) {
            case 'FLOPPY_SCALE':
                return 'Floppy-scale';
            case 'CD_R_650':
                return 'CD-R 650';
            case 'CD_R_700':
                return 'CD-R 700';
            case 'HDS_1_GIB':
                return '1 GiB';
            case 'HDS_2_GIB':
                return '2 GiB';
        }
    }

    function normalizedFileName(): string | null {
        let stem = fileName.trim();
        if (stem.toLocaleLowerCase().endsWith('.hds')) stem = stem.slice(0, -4).trimEnd();
        if (!stem || stem === '.' || stem === '..' || stem.includes('/') || stem.includes('\\')) {
            error = 'Enter a filename without directory separators';
            return null;
        }
        return `${stem}.hds`;
    }

    function outputLocation(name: string): FileLocation {
        const parent = directory.reference.relativePath;
        const relativePath = parent ? `${parent}/${name}` : name;
        return serverFileLocation(
            { rootId: directory.reference.rootId, relativePath },
            `${directory.displayName}/${name}`,
        );
    }

    async function createImage(event: SubmitEvent): Promise<void> {
        event.preventDefault();
        if (busy || !selectedProfile || !selectedOption) return;
        const name = normalizedFileName();
        if (!name) return;
        const output = outputLocation(name);
        busy = true;
        error = '';
        try {
            const plan = await transport.planHardDiskCreation(profileId, partitionCount, output);
            if (!plan.planToken) throw new Error('The server did not return a build plan token');
            activeJob = await transport.startHardDiskCreation(plan.planToken);
            const completed = await transport.waitForJob(activeJob.jobId, (job) => (activeJob = job));
            if (completed.status === 'failed') throw new Error(completed.error ?? 'Hard-disk image creation failed');
            if (completed.status !== 'completed') throw new Error('Hard-disk image creation did not complete');
            onsuccess(output);
        } catch (reason) {
            error = userFacingMessage(reason);
        } finally {
            activeJob = null;
            busy = false;
        }
    }

    async function cancel(): Promise<void> {
        if (activeJob) {
            await transport.cancelJob(activeJob.jobId);
        }
        oncancel();
    }

    function formatBytes(bytes: number): string {
        if (bytes >= 1024 ** 3) return `${(bytes / 1024 ** 3).toFixed(2)} GiB`;
        const mebibytes = bytes / 1024 ** 2;
        return `${mebibytes < 10 ? mebibytes.toFixed(2) : Math.round(mebibytes)} MiB`;
    }
</script>

<div class="dialog-backdrop dialog-backdrop-raised" role="presentation">
    <div class="dialog-shell create-hds-dialog" role="dialog" aria-modal="true" aria-label="Create HD image">
        <header class="dialog-header">
            <h2>Create HD image</h2>
            <button
                class="icon-button"
                type="button"
                aria-label="Close"
                disabled={busy && !activeJob}
                onclick={() => void cancel()}
            >
                <Icon name="close" size={15} />
            </button>
        </header>

        <form onsubmit={createImage}>
            <label class="create-hds-field">
                <span>Location</span>
                <output>{directory.displayName}</output>
            </label>
            <label class="create-hds-field">
                <span>File name</span>
                <span class="create-hds-name"
                    ><input aria-label="File name" bind:value={fileName} disabled={busy} /><b>.hds</b></span
                >
            </label>

            <fieldset disabled={busy || loading}>
                <legend>Capacity</legend>
                <div class="create-hds-segments capacity-segments">
                    {#each profiles as profile (profile.profileId)}
                        <button
                            type="button"
                            aria-pressed={profileId === profile.profileId}
                            onclick={() => selectProfile(profile.profileId)}>{profileLabel(profile.profileId)}</button
                        >
                    {/each}
                </div>
            </fieldset>

            <fieldset disabled={busy || loading}>
                <legend>Partitions</legend>
                <div class="create-hds-segments partition-segments">
                    {#each partitionCounts as count (count)}
                        {@const option = selectedProfile?.partitionOptions.find(
                            (candidate) => candidate.partitionCount === count,
                        )}
                        <button
                            type="button"
                            aria-label={`${count} ${count === 1 ? 'partition' : 'partitions'}`}
                            aria-pressed={Boolean(option && partitionCount === count)}
                            disabled={!option}
                            onclick={() => {
                                if (option) partitionCount = count;
                            }}>{count}</button
                        >
                    {/each}
                </div>
            </fieldset>

            {#if selectedProfile && selectedOption}
                <p class="create-hds-summary">
                    {partitionCount}
                    {partitionCount === 1 ? 'partition' : 'partitions'} · {formatBytes(
                        selectedOption.partitionSizeBytes,
                    )} each
                </p>
            {/if}
            {#if error}<p class="create-hds-error" role="alert">{error}</p>{/if}

            <footer class="dialog-footer">
                <button
                    class="secondary-button"
                    type="button"
                    disabled={busy && !activeJob}
                    onclick={() => void cancel()}>Cancel</button
                >
                <button
                    class="primary-button"
                    type="submit"
                    disabled={loading || busy || !selectedProfile || !selectedOption}
                >
                    {busy ? 'Creating' : 'Create'}
                </button>
            </footer>
        </form>
    </div>
</div>

<style>
    .create-hds-dialog {
        width: min(620px, calc(100vw - 40px));
    }

    form {
        display: grid;
        gap: 10px;
        padding: 11px 12px 0;
    }

    .create-hds-field {
        display: grid;
        gap: 4px;
        color: var(--color-text-muted);
        font-size: 9px;
    }

    .create-hds-field output,
    .create-hds-name {
        min-width: 0;
        height: 30px;
        display: flex;
        align-items: center;
        overflow: hidden;
        color: var(--color-text-strong);
        border: 1px solid var(--color-border);
        border-radius: 6px;
        background: var(--color-bg-deep);
        font-family: var(--font-mono);
        font-size: 10px;
        text-overflow: ellipsis;
        white-space: nowrap;
    }

    .create-hds-field output {
        padding: 0 9px;
    }

    .create-hds-name input {
        min-width: 0;
        height: 100%;
        flex: 1;
        padding: 0 9px;
        color: inherit;
        border: 0;
        outline: 0;
        background: transparent;
    }

    .create-hds-name b {
        padding-right: 9px;
        color: var(--color-text-muted);
        font-weight: 500;
    }

    fieldset {
        min-width: 0;
        margin: 0;
        padding: 0;
        border: 0;
    }

    legend {
        margin-bottom: 4px;
        color: var(--color-text-muted);
        font-size: 9px;
    }

    .create-hds-segments {
        display: grid;
        gap: 1px;
        overflow: hidden;
        border: 1px solid var(--color-border);
        border-radius: 6px;
        background: var(--color-border);
    }

    .capacity-segments {
        grid-template-columns: repeat(5, minmax(0, 1fr));
    }

    .partition-segments {
        grid-template-columns: repeat(8, minmax(0, 1fr));
    }

    .create-hds-segments button {
        min-width: 0;
        height: 30px;
        padding: 0 5px;
        overflow: hidden;
        color: var(--color-text);
        border: 0;
        background: var(--color-panel);
        cursor: pointer;
        font-size: 10px;
        text-overflow: ellipsis;
        white-space: nowrap;
    }

    .create-hds-segments button[aria-pressed='true'] {
        color: var(--color-text-strong);
        background: var(--color-accent-strong);
    }

    .create-hds-segments button:disabled {
        color: var(--color-text-dim);
        background: var(--color-bg-deep);
        cursor: default;
        opacity: 0.55;
    }

    .create-hds-summary {
        margin: 0;
        color: var(--color-text-muted);
        font-size: 9px;
    }

    .create-hds-error {
        margin: 0;
        padding: 7px 9px;
        color: #f1a4a4;
        border: 1px solid rgb(190 80 80 / 35%);
        border-radius: 5px;
        background: rgb(120 35 35 / 20%);
        font-size: 9px;
    }

    footer {
        margin: 0 -12px;
    }

    @media (max-width: 560px) {
        .capacity-segments {
            grid-template-columns: repeat(3, minmax(0, 1fr));
        }
    }
</style>
