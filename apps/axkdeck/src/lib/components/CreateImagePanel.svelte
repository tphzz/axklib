<script lang="ts">
    import { onMount } from 'svelte';
    import { serverFileLocation, type DirectoryLocation, type FileLocation } from '../storageLocations';
    import type { HardDiskCreationProfile, HardDiskCreationProfileId, ImageTransport, JobState } from '../transport';
    import { userFacingMessage } from '../userFacingMessage';
    import Icon from './Icon.svelte';
    import SelectControl from './SelectControl.svelte';
    import SfsPartitionControls from './SfsPartitionControls.svelte';

    interface Props {
        transport: ImageTransport;
        directory: DirectoryLocation | null;
        onbrowse?: () => void;
        onstatechange?: (state: { busy: boolean; cancellable: boolean; cancel: () => Promise<void> }) => void;
        onsuccess: (file: FileLocation) => void;
        oncancel: () => void;
    }

    let { transport, directory, onsuccess, oncancel, onbrowse, onstatechange }: Props = $props();
    type ImageType = 'HD' | 'Floppy';

    let profiles = $state<HardDiskCreationProfile[]>([]);
    let imageType = $state<ImageType>('HD');
    let profileId = $state<HardDiskCreationProfileId>('FLOPPY_SCALE');
    let partitionCount = $state(1);
    let fileName = $state('New disk');
    let loading = $state(true);
    let busy = $state(false);
    let cancelling = $state(false);
    let activeJob = $state<JobState | null>(null);
    let error = $state('');
    $effect(() => onstatechange?.({ busy, cancellable: !busy || !!activeJob, cancel }));

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
                return '1.44 MB';
            case 'HDS_128_MIB':
                return '128 MiB';
            case 'HDS_256_MIB':
                return '256 MiB';
            case 'CD_R_650':
                return 'CD-R 650';
            case 'CD_R_700':
                return 'CD-R 700';
            case 'HDS_1_GIB':
                return '1 GiB';
            case 'HDS_2_GIB':
                return '2 GiB';
            case 'HDS_4_GIB':
                return '4 GiB';
            case 'HDS_8_GIB':
                return '8 GiB';
        }
    }

    function normalizedFileName(): string | null {
        let stem = fileName.trim();
        const lower = stem.toLocaleLowerCase();
        if (lower.endsWith('.hds') || lower.endsWith('.ima')) stem = stem.slice(0, -4).trimEnd();
        if (!stem || stem === '.' || stem === '..' || stem.includes('/') || stem.includes('\\')) {
            error = 'Enter a filename without directory separators';
            return null;
        }
        return `${stem}${imageType === 'Floppy' ? '.ima' : '.hds'}`;
    }

    function outputLocation(name: string): FileLocation {
        if (!directory) throw new Error('Choose an image location');
        const parent = directory.reference.relativePath;
        const relativePath = parent ? `${parent}/${name}` : name;
        return serverFileLocation(
            { rootId: directory.reference.rootId, relativePath },
            `${directory.displayName}/${name}`,
        );
    }

    async function createImage(event: SubmitEvent): Promise<void> {
        event.preventDefault();
        if (busy || !directory || (imageType === 'HD' && (!selectedProfile || !selectedOption))) return;
        const name = normalizedFileName();
        if (!name) return;
        const output = outputLocation(name);
        busy = true;
        error = '';
        try {
            const plan =
                imageType === 'Floppy'
                    ? await transport.planFloppyCreation(output)
                    : await transport.planHardDiskCreation(profileId, partitionCount, output);
            if (!plan.planToken) throw new Error('The server did not return a build plan token');
            activeJob =
                imageType === 'Floppy'
                    ? await transport.startFloppyCreation(plan.planToken)
                    : await transport.startHardDiskCreation(plan.planToken);
            const completed = await transport.waitForJob(activeJob.jobId, (job) => (activeJob = job));
            if (completed.status === 'cancelled') {
                oncancel();
                return;
            }
            if (completed.status === 'failed') throw new Error(completed.error ?? 'Image creation failed');
            if (completed.status !== 'completed') throw new Error('Image creation did not complete');
            onsuccess(output);
        } catch (reason) {
            error = userFacingMessage(reason);
        } finally {
            activeJob = null;
            busy = false;
        }
    }

    async function cancel(): Promise<void> {
        if (busy && !activeJob) return;
        if (activeJob) {
            if (cancelling) return;
            cancelling = true;
            error = '';
            try {
                await transport.cancelJob(activeJob.jobId);
            } catch (reason) {
                cancelling = false;
                error = userFacingMessage(reason);
            }
            return;
        }
        oncancel();
    }
</script>

<form aria-label="Create image" onsubmit={createImage}>
    <div class="create-image-content">
        <label class="create-hds-field">
            <span>Location</span>
            <span class="location"
                ><output>{directory?.displayName ?? 'Choose an image location'}</output>
                {#if onbrowse}<button class="secondary-button" type="button" disabled={busy} onclick={onbrowse}
                        ><Icon name="folder-open" size={14} /> Browse</button
                    >{/if}</span
            >
        </label>
        <label class="create-hds-field">
            <span>Type</span>
            <SelectControl
                label="Type"
                value={imageType}
                options={[
                    { value: 'HD', label: 'HD' },
                    { value: 'Floppy', label: 'Floppy' },
                ] as const}
                disabled={busy}
                onchange={(value) => {
                    imageType = value;
                    error = '';
                }}
            />
        </label>
        <label class="create-hds-field">
            <span>File name</span>
            <span class="create-hds-name"
                ><input
                    class="dialog-field-control"
                    aria-label="File name"
                    bind:value={fileName}
                    data-dialog-initial-focus="select"
                    disabled={busy}
                /><b>{imageType === 'Floppy' ? '.ima' : '.hds'}</b></span
            >
        </label>

        {#if imageType === 'HD'}
            <fieldset disabled={busy || loading}>
                <legend>Capacity</legend>
                <SelectControl
                    label="Capacity"
                    value={profileId}
                    options={profiles.map((profile) => ({
                        value: profile.profileId,
                        label: profileLabel(profile.profileId),
                    }))}
                    disabled={busy || loading}
                    onchange={selectProfile}
                />
            </fieldset>

            <SfsPartitionControls
                options={selectedProfile?.partitionOptions ?? []}
                value={partitionCount}
                disabled={busy || loading}
                onchange={(value) => (partitionCount = value)}
            />
        {/if}

        {#if imageType === 'Floppy'}
            <p class="create-hds-summary">1.44 MB · Yamaha A-series FAT12 · Full format</p>
        {/if}
        {#if error}<p class="create-hds-error" role="alert">{error}</p>{/if}
    </div>

    <footer class="dialog-footer">
        <span class="dialog-footer-status" role="status"
            >{cancelling
                ? 'Cancelling...'
                : busy
                  ? activeJob?.progress?.label || 'Creating image...'
                  : loading
                    ? 'Loading profiles...'
                    : ''}</span
        >
        <div class="dialog-footer-actions">
            <button class="secondary-button" type="button" disabled={busy && !activeJob} onclick={() => void cancel()}
                >Cancel</button
            >
            <button
                class="primary-button"
                type="submit"
                disabled={busy ||
                    !directory ||
                    (imageType === 'HD' && (loading || !selectedProfile || !selectedOption))}
            >
                Create
            </button>
        </div>
    </footer>
</form>

<style>
    form {
        display: flex;
        flex-direction: column;
        flex: 1;
        min-height: 0;
    }
    .create-image-content {
        display: grid;
        flex: 1;
        align-content: start;
        gap: 10px;
        padding: 11px 12px;
        overflow: auto;
        min-height: 0;
    }
    .location {
        display: flex;
        gap: 8px;
        min-width: 0;
    }
    .location output {
        flex: 1;
    }

    .create-hds-field {
        display: grid;
        gap: 4px;
        color: var(--color-text-muted);
        font-size: var(--dialog-label-font-size);
    }

    .create-hds-field output {
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
        font-size: var(--dialog-control-font-size);
        text-overflow: ellipsis;
        white-space: nowrap;
    }

    .create-hds-field output {
        padding: 0 9px;
    }

    .create-hds-name {
        display: flex;
        align-items: center;
        gap: 8px;
    }

    .create-hds-name input {
        min-width: 0;
        flex: 1;
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
        font-size: var(--dialog-label-font-size);
    }

    .create-hds-summary {
        margin: 0;
        color: var(--color-text-muted);
        font-size: var(--dialog-metadata-font-size);
    }

    .create-hds-error {
        margin: 0;
        padding: 7px 9px;
        color: #f1a4a4;
        border: 1px solid rgb(190 80 80 / 35%);
        border-radius: 5px;
        background: rgb(120 35 35 / 20%);
        font-size: var(--dialog-body-font-size);
    }
</style>
