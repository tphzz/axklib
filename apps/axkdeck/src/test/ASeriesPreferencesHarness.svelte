<script lang="ts">
    import { onMount } from 'svelte';
    import {
        ASeriesPreferences,
        initialBankSampleFormat,
        type KnownSampleFormat,
    } from '../lib/aSeriesPreferences.svelte';
    import type { SampleStorageFormat } from '../lib/objectEditing';
    import type { HardDiskCreationProfile, HardDiskCreationProfileId, ImageTransport } from '../lib/transport';
    import { serverDirectoryLocation } from '../lib/storageLocations';
    import PreferencesDialog from '../lib/components/PreferencesDialog.svelte';
    import AssignSampleBankDialog from '../lib/components/AssignSampleBankDialog.svelte';
    import CreateHardDiskImageDialog from '../lib/components/CreateHardDiskImageDialog.svelte';
    import { sampleFormatFixture } from './sampleFormatFixture';

    let ready = $state(false);
    let dialog = $state<'preferences' | 'bank' | 'image' | null>(null);
    let initialSampleFormat = $state<KnownSampleFormat>('A3000_188');
    let preferenceWrites = $state(0);
    let failNextSave = $state(false);
    let bankSubmission = $state('');
    let creation = $state('');
    const preferences = new ASeriesPreferences({
        load: async () => 'A3000',
        save: async () => {
            if (failNextSave) {
                failNextSave = false;
                throw new Error('Test preference save failure');
            }
            preferenceWrites++;
        },
    });
    onMount(async () => {
        await preferences.ready;
        ready = true;
    });
    const banks = [
        { name: 'Native Bank', objectId: 'native', format: 'A3000_188' as const },
        { name: 'Later Bank', objectId: 'later', format: 'A4000_A5000_224' as const },
    ].map(({ name, objectId, format }) => ({
        objectId,
        name,
        memberCount: 2,
        selectedMemberCount: 0,
        movedSampleCount: 2,
        reassignedSampleCount: 0,
        finalMemberCount: 4,
        sampleFormat: sampleFormatFixture(format).sampleFormat,
    }));
    function openBank(formats: SampleStorageFormat[]): void {
        initialSampleFormat = initialBankSampleFormat(formats, preferences.generation);
        dialog = 'bank';
    }
    function profile(
        profileId: HardDiskCreationProfileId,
        sizeBytes: number,
        minimum: number,
        maximum = 8,
    ): HardDiskCreationProfile {
        return {
            profileId,
            sizeBytes,
            defaultPartitionCount: minimum,
            partitionOptions: Array.from({ length: maximum - minimum + 1 }, (_, index) => {
                const partitionCount = minimum + index;
                const slotSectors = Math.min(Math.floor((sizeBytes / 512 - 2) / partitionCount), 0x1fffff);
                return {
                    partitionCount,
                    partitionSizeBytes: (slotSectors - 1) * 512,
                    unusedTailBytes: sizeBytes - (2 + partitionCount * slotSectors) * 512,
                };
            }),
        };
    }
    const transport = {
        hardDiskCreationProfiles: async () => [
            profile('FLOPPY_SCALE', 1_474_560, 1, 1),
            profile('HDS_128_MIB', 128 * 1024 ** 2, 1),
            profile('HDS_256_MIB', 256 * 1024 ** 2, 1),
            profile('CD_R_650', 681_984_000, 1),
            profile('CD_R_700', 737_280_000, 1),
            profile('HDS_1_GIB', 1024 ** 3, 1),
            profile('HDS_2_GIB', 2 * 1024 ** 3, 2),
            profile('HDS_4_GIB', 4 * 1024 ** 3, 4),
            profile('HDS_8_GIB', 8 * 1024 ** 3, 8),
        ],
        planHardDiskCreation: async (profileId: HardDiskCreationProfileId, partitionCount: number) => {
            creation = JSON.stringify({ profileId, partitionCount });
            return { planToken: 'mock-plan' };
        },
        startHardDiskCreation: async () => ({ jobId: 1, status: 'queued' }),
        waitForJob: async () => ({ jobId: 1, status: 'completed' }),
    } as unknown as ImageTransport;
</script>

<main aria-label="A-Series regression controls">
    <div class="harness-actions">
        <button disabled={!ready} onclick={() => (dialog = 'preferences')}>Open preferences</button>
        <button disabled={!ready} onclick={() => openBank(['A3000_188'])}>Assign native Samples</button>
        <button disabled={!ready} onclick={() => openBank(['A3000_188', 'A4000_A5000_224'])}
            >Assign mixed Samples</button
        >
        <button disabled={!ready} onclick={() => openBank(['UNKNOWN'])}>Assign unknown Samples</button>
        <button disabled={!ready} onclick={() => (dialog = 'image')}>Create image</button>
    </div>
    <label><input type="checkbox" bind:checked={failNextSave} />Fail next preference save</label>
    <output aria-label="Saved generation">{preferences.generation}</output>
    <output aria-label="Preference writes">{preferenceWrites}</output>
    <output aria-label="Bank submission">{bankSubmission}</output>
    <output aria-label="Image creation">{creation}</output>
</main>

{#if dialog === 'preferences'}
    <PreferencesDialog {preferences} oncancel={() => (dialog = null)} onsaved={() => (dialog = null)} />
{:else if dialog === 'bank'}
    <AssignSampleBankDialog
        volumeName="Samples"
        sampleCount={2}
        assignedSampleCount={0}
        {initialSampleFormat}
        options={banks}
        blockers={[]}
        busy={false}
        error=""
        oncancel={() => (dialog = null)}
        onsubmit={(target) => {
            bankSubmission = JSON.stringify(target);
            dialog = null;
        }}
    />
{:else if dialog === 'image'}
    <CreateHardDiskImageDialog
        {transport}
        directory={serverDirectoryLocation({ rootId: 'mock', relativePath: 'images' }, 'Synthetic images')}
        oncancel={() => (dialog = null)}
        onsuccess={() => (dialog = null)}
    />
{/if}

<style>
    main {
        display: grid;
        gap: 12px;
        padding: 16px;
        font-size: 11px;
    }
    .harness-actions {
        display: flex;
        flex-wrap: wrap;
        gap: 8px;
    }
    output {
        overflow-wrap: anywhere;
    }
</style>
