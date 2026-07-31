import { fireEvent, render, screen, within } from '@testing-library/svelte';
import { beforeEach, describe, expect, it, vi } from 'vitest';

const mocks = vi.hoisted(() => ({
    sandboxRoots: vi.fn(),
    sandboxDirectory: vi.fn(),
    inspectSandboxMediaSource: vi.fn(),
    openImage: vi.fn(),
    refreshImage: vi.fn(),
    attachCompanionDirectories: vi.fn(),
    closeImage: vi.fn(),
    contentChildren: vi.fn(),
    objectPage: vi.fn(),
    relationshipPage: vi.fn(),
    preview: vi.fn(),
    inspectPackage: vi.fn(),
    planImagePackageImport: vi.fn(),
    releaseImagePackageImportPlan: vi.fn(),
    releaseClientUpload: vi.fn(),
    inspectObjectDeletion: vi.fn(),
    inspectWaveDataOrphans: vi.fn(),
    startObjectDeletion: vi.fn(),
    startObjectRename: vi.fn(),
    waitForJob: vi.fn(),
    deleteSandboxEntry: vi.fn(),
    hardDiskCreationProfiles: vi.fn(),
    listenForNativeMediaDrops: vi.fn(),
    audioImportCapabilities: vi.fn(),
    inspectAudio: vi.fn(),
    uploadClientFile: vi.fn(),
}));

vi.mock('./lib/createTransport', () => ({
    createTransport: () => ({
        storageMode: 'server',
        supportsClientUploads: true,
        sandboxRoots: mocks.sandboxRoots,
        sandboxDirectory: mocks.sandboxDirectory,
        inspectSandboxMediaSource: mocks.inspectSandboxMediaSource,
        openImage: mocks.openImage,
        refreshImage: mocks.refreshImage,
        attachCompanionDirectories: mocks.attachCompanionDirectories,
        closeImage: mocks.closeImage,
        contentChildren: mocks.contentChildren,
        objectPage: mocks.objectPage,
        relationshipPage: mocks.relationshipPage,
        preview: mocks.preview,
        inspectPackage: mocks.inspectPackage,
        planImagePackageImport: mocks.planImagePackageImport,
        releaseImagePackageImportPlan: mocks.releaseImagePackageImportPlan,
        releaseClientUpload: mocks.releaseClientUpload,
        inspectObjectDeletion: mocks.inspectObjectDeletion,
        inspectWaveDataOrphans: mocks.inspectWaveDataOrphans,
        startObjectDeletion: mocks.startObjectDeletion,
        startObjectRename: mocks.startObjectRename,
        waitForJob: mocks.waitForJob,
        deleteSandboxEntry: mocks.deleteSandboxEntry,
        hardDiskCreationProfiles: mocks.hardDiskCreationProfiles,
        audioImportCapabilities: mocks.audioImportCapabilities,
        inspectAudio: mocks.inspectAudio,
        uploadClientFile: mocks.uploadClientFile,
    }),
}));

vi.mock('./lib/nativeMediaDrop', () => ({
    listenForNativeMediaDrops: mocks.listenForNativeMediaDrops,
}));

import App from './App.svelte';
import { AuditionController } from './lib/audio/auditionController';

async function chooseNestedImage(buttonName: 'Open image' | 'Open another image' = 'Open image'): Promise<void> {
    await fireEvent.click(screen.getByRole('button', { name: buttonName }));
    const picker = await screen.findByRole('dialog', { name: 'Open image' });
    if (buttonName === 'Open image') {
        await fireEvent.click(await within(picker).findByText('Yamaha'));
        await fireEvent.click(await within(picker).findByText('images'));
    }
    await fireEvent.click(await within(picker).findByText('nested.hds'));
}

describe('App panel layout', () => {
    beforeEach(() => {
        delete window.__AXKLIB_SERVER__;
        mocks.sandboxRoots.mockReset().mockResolvedValue([{ id: 'workspace', displayName: 'Yamaha', writable: true }]);
        mocks.sandboxDirectory.mockReset().mockImplementation(async (directory) => ({
            directory,
            entries:
                directory.relativePath === 'images'
                    ? [{ name: 'nested.hds', relativePath: 'images/nested.hds', kind: 'FILE', size: 2048 }]
                    : [{ name: 'images', relativePath: 'images', kind: 'DIRECTORY', size: null }],
            truncated: false,
            nextCursor: null,
        }));
        mocks.inspectSandboxMediaSource.mockReset().mockResolvedValue(null);
        mocks.hardDiskCreationProfiles.mockReset().mockResolvedValue([
            {
                profileId: 'FLOPPY_SCALE',
                sizeBytes: 1474560,
                defaultPartitionCount: 1,
                partitionOptions: [{ partitionCount: 1, partitionSizeBytes: 1454080, unusedTailBytes: 0 }],
            },
        ]);
        mocks.openImage.mockReset().mockResolvedValue({
            sessionId: 17,
            companionDirectories: [],
            tree: [{ id: 'disk-17', name: 'nested.hds', kind: 'disk', childCount: 0 }],
            validation: {
                valid: true,
                issueCount: 0,
                errorCount: 0,
                warningCount: 0,
                objectCount: 0,
                relationshipCount: 0,
            },
            objects: [],
            objectTotalCount: 0,
            initialVolume: null,
            volumeMutationsAvailable: true,
            partitionMutationsAvailable: true,
            objectRenameAvailable: true,
            objectDeletionAvailable: true,
            waveDataCleanupAvailable: false,
        });
        mocks.refreshImage.mockReset();
        mocks.attachCompanionDirectories.mockReset();
        mocks.closeImage.mockReset().mockResolvedValue(undefined);
        mocks.contentChildren.mockReset().mockResolvedValue({ items: [], totalCount: 0 });
        mocks.objectPage.mockReset().mockResolvedValue({ objects: [], totalCount: 0 });
        mocks.relationshipPage.mockReset().mockResolvedValue({ relationships: [], totalCount: 0 });
        mocks.preview.mockReset().mockResolvedValue({ frameCount: 1, lanes: [] });
        mocks.inspectPackage.mockReset();
        mocks.planImagePackageImport.mockReset();
        mocks.releaseImagePackageImportPlan.mockReset().mockResolvedValue(undefined);
        mocks.releaseClientUpload.mockReset().mockResolvedValue(undefined);
        mocks.inspectObjectDeletion.mockReset();
        mocks.inspectWaveDataOrphans.mockReset();
        mocks.startObjectDeletion.mockReset();
        mocks.startObjectRename.mockReset();
        mocks.waitForJob.mockReset();
        mocks.deleteSandboxEntry.mockReset().mockResolvedValue(undefined);
        mocks.listenForNativeMediaDrops.mockReset().mockResolvedValue(() => undefined);
        mocks.audioImportCapabilities.mockReset().mockResolvedValue({
            supportedSampleRates: [44_100],
            defaultUnsupportedSampleRate: 44_100,
            supportedOutputSampleWidthsBits: [16],
            sampleWidthPolicy: 'PRESERVE_PCM16_EXPAND_PCM8',
        });
        mocks.inspectAudio.mockReset().mockResolvedValue({
            sourceFormat: 'WAV',
            sourceSubtype: 'PCM_16',
            channels: 1,
            frameCount: 48_000,
            sourceSampleRate: 44_100,
            outputSampleRate: 44_100,
            sourceSampleWidthBits: 16,
            outputSampleWidthBits: 16,
            durationSeconds: 1,
            resampled: false,
            quantized: false,
            sampleWidthConverted: false,
            ditherAlgorithm: 'NONE',
            projectedOutputFrameCount: 48_000,
            projectedOutputBytesPerChannel: 96_000,
            projectedOutputBytesTotal: 96_000,
            maximumOutputFrameCountPerChannel: 16_777_216,
            maximumOutputBytesPerChannel: 33_554_432,
            samplerDefaults: {
                rootKey: 60,
                fineTuneCents: 0,
                keyLow: 0,
                keyHigh: 127,
                velocityLow: 0,
                velocityHigh: 127,
                loopMode: 4,
                loopStartFrame: 0,
                loopLengthFrames: 0,
                pitchSource: 'DEFAULT',
                rangeSource: 'DEFAULT',
                loopSource: 'DEFAULT',
            },
            valid: true,
            issues: [],
        });
        mocks.uploadClientFile.mockReset();
    });

    it('keeps one stable toolbar across all side-panel combinations', async () => {
        const { container } = render(App);
        const shell = container.querySelector('.app-shell');
        const toolbar = screen.getByRole('toolbar', { name: 'Panel layout' });
        const library = screen.getByRole('button', { name: 'Library panel' });
        const inspector = screen.getByRole('button', { name: 'Inspector panel' });
        const editor = screen.getByRole('button', { name: 'Editor panel' });

        expect(shell?.classList.contains('sidebar-closed')).toBe(false);
        expect(shell?.classList.contains('inspector-closed')).toBe(false);
        expect(screen.queryByRole('region', { name: 'Object editor' })).toBeNull();
        expect(editor.getAttribute('aria-pressed')).toBe('false');

        await fireEvent.click(editor);
        expect(screen.getByRole('region', { name: 'Object editor' })).toBeTruthy();
        expect(editor.getAttribute('aria-pressed')).toBe('true');
        await fireEvent.click(editor);
        expect(screen.queryByRole('region', { name: 'Object editor' })).toBeNull();
        expect(editor.getAttribute('aria-pressed')).toBe('false');

        await fireEvent.click(library);
        expect(screen.getByRole('toolbar', { name: 'Panel layout' })).toBe(toolbar);
        expect(shell?.classList.contains('sidebar-closed')).toBe(true);
        expect(shell?.classList.contains('inspector-closed')).toBe(false);

        await fireEvent.click(inspector);
        expect(screen.getByRole('toolbar', { name: 'Panel layout' })).toBe(toolbar);
        expect(shell?.classList.contains('sidebar-closed')).toBe(true);
        expect(shell?.classList.contains('inspector-closed')).toBe(true);

        await fireEvent.click(library);
        expect(shell?.classList.contains('sidebar-closed')).toBe(false);
        expect(shell?.classList.contains('inspector-closed')).toBe(true);

        await fireEvent.click(inspector);
        expect(shell?.classList.contains('sidebar-closed')).toBe(false);
        expect(shell?.classList.contains('inspector-closed')).toBe(false);
    });

    it('uses canonical Yamaha object terminology', () => {
        render(App);

        expect(screen.getByRole('button', { name: 'Sample Banks' })).toBeTruthy();
        expect(screen.getByRole('button', { name: 'Samples' })).toBeTruthy();
        expect(screen.getByRole('button', { name: 'Wave Data' })).toBeTruthy();
        expect(screen.queryByText('Sample pool')).toBeNull();
    });

    it('keeps autoplay session-local and disabled until the user enables it', async () => {
        render(App);

        expect(screen.queryByRole('checkbox', { name: 'Autoplay' })).toBeNull();
        await fireEvent.click(screen.getByRole('button', { name: 'Samples' }));
        const autoplay = screen.getByRole('checkbox', { name: 'Autoplay' }) as HTMLInputElement;
        expect(autoplay.checked).toBe(false);
        await fireEvent.click(autoplay);
        expect(autoplay.checked).toBe(true);
    });

    it('uses contained-object lanes above the editor for SBAC and SBNK views', async () => {
        const { container } = render(App);
        await fireEvent.click(screen.getByRole('button', { name: 'Editor panel' }));

        await fireEvent.click(screen.getByRole('button', { name: 'Sample Banks' }));
        expect(screen.getByRole('region', { name: 'Sample Bank hierarchy' })).toBeTruthy();
        expect(document.querySelectorAll('.contained-lane')).toHaveLength(3);
        expect(container.querySelector('.object-editor')?.textContent).toContain('No object selected');

        await fireEvent.click(screen.getByRole('button', { name: 'Samples' }));
        expect(screen.getByRole('region', { name: 'Sample hierarchy' })).toBeTruthy();
        expect(document.querySelectorAll('.contained-lane')).toHaveLength(2);
        expect(container.querySelector('.object-editor')?.textContent).toContain('No object selected');
    });

    it('schedules gapless Sample Bank playback in the natural order displayed in the Samples lane', async () => {
        const volume = {
            id: 'volume-1',
            name: 'Slices',
            kind: 'volume' as const,
            childCount: 0,
            partitionIndex: 0,
        };
        const samplerObject = (key: string, objectType: string, name: string) => ({
            key,
            objectType,
            name,
            partitionIndex: 0,
            partitionName: 'Partition 0',
            volumeName: volume.name,
            categoryName: objectType,
            sfsId: 1,
            storedSizeBytes: 2,
            sampleRate: objectType === 'SMPL' ? 44_100 : 0,
            rootKey: 60,
            frameCount: objectType === 'SMPL' ? 1 : 0,
            sampleWidthBytes: objectType === 'SMPL' ? 2 : 0,
        });
        const bank = samplerObject('SBAC-1', 'SBAC', 'Slice Bank');
        const sample00 = samplerObject('SBNK-00', 'SBNK', 'LoopDiv00');
        const sample02 = samplerObject('SBNK-02', 'SBNK', 'LoopDiv02');
        const sample10 = samplerObject('SBNK-10', 'SBNK', 'LoopDiv10');
        const wave00 = samplerObject('SMPL-00', 'SMPL', 'Wave 00');
        const wave02 = samplerObject('SMPL-02', 'SMPL', 'Wave 02');
        const wave10 = samplerObject('SMPL-10', 'SMPL', 'Wave 10');
        const relationship = (
            id: string,
            sourceObjectId: string,
            targetObjectId: string,
            relationshipType: string,
            assignmentIndex?: number,
        ) => ({
            id,
            sourceObjectId,
            targetObjectId,
            candidateObjectIds: [],
            relationshipType,
            quality: 'KNOWN',
            basis: 'test',
            notes: [],
            assignmentIndex,
            assignmentName: '',
            assignmentState: '',
            receiveChannelDisplay: '',
        });
        const relationships = [
            relationship('bank-10', bank.key, sample10.key, 'SBAC_SLOT_TO_SBNK', 1),
            relationship('bank-02', bank.key, sample02.key, 'SBAC_SLOT_TO_SBNK', 2),
            relationship('bank-00', bank.key, sample00.key, 'SBAC_SLOT_TO_SBNK', 3),
            relationship('wave-10', sample10.key, wave10.key, 'SBNK_LEFT_MEMBER_TO_SMPL'),
            relationship('wave-02', sample02.key, wave02.key, 'SBNK_LEFT_MEMBER_TO_SMPL'),
            relationship('wave-00', sample00.key, wave00.key, 'SBNK_LEFT_MEMBER_TO_SMPL'),
        ];
        mocks.openImage.mockResolvedValueOnce({
            sessionId: 17,
            tree: [{ id: 'disk-17', name: 'nested.hds', kind: 'disk', childCount: 1, children: [volume] }],
            validation: {
                valid: true,
                issueCount: 0,
                errorCount: 0,
                warningCount: 0,
                objectCount: 7,
                relationshipCount: relationships.length,
            },
            objects: [],
            objectTotalCount: 0,
            initialVolume: volume,
            volumeMutationsAvailable: true,
            partitionMutationsAvailable: true,
            objectDeletionAvailable: true,
        });
        mocks.objectPage.mockResolvedValue({
            objects: [bank, sample10, sample02, sample00, wave10, wave02, wave00],
            totalCount: 7,
        });
        mocks.relationshipPage.mockResolvedValue({ relationships, totalCount: relationships.length });
        const playSequence = vi.spyOn(AuditionController.prototype, 'playSequence').mockImplementation(() => undefined);

        try {
            render(App);
            await chooseNestedImage();
            await fireEvent.click(screen.getByRole('button', { name: 'Sample Banks' }));
            await fireEvent.click(await screen.findByRole('button', { name: 'Play Slice Bank' }));

            expect(
                screen
                    .getAllByRole('button', { name: /^Inspect LoopDiv/ })
                    .map((button) => button.getAttribute('aria-label')),
            ).toEqual(['Inspect LoopDiv00', 'Inspect LoopDiv02', 'Inspect LoopDiv10']);
            expect(playSequence).toHaveBeenCalledWith(
                17,
                [sample00.key, sample02.key, sample10.key],
                expect.any(Function),
                bank.key,
            );
        } finally {
            playSequence.mockRestore();
        }
    });

    it('attaches nearby companion folders and retries only after an explicit playback failure', async () => {
        const volume = {
            id: 'volume-1',
            name: 'Slices',
            kind: 'volume' as const,
            childCount: 0,
            partitionIndex: 0,
        };
        const samplerObject = (key: string, objectType: string, name: string) => ({
            key,
            objectType,
            name,
            partitionIndex: 0,
            partitionName: 'Partition 0',
            volumeName: volume.name,
            categoryName: objectType,
            sfsId: 1,
            storedSizeBytes: 2,
            sampleRate: objectType === 'SMPL' ? 44_100 : 0,
            rootKey: 60,
            frameCount: objectType === 'SMPL' ? 1 : 0,
            sampleWidthBytes: objectType === 'SMPL' ? 2 : 0,
        });
        const bank = samplerObject('SBAC-1', 'SBAC', 'Slice Bank');
        const sample = samplerObject('SBNK-1', 'SBNK', 'Slice 1');
        const wave = samplerObject('SMPL-1', 'SMPL', 'Wave 1');
        const relationships = [
            {
                id: 'bank-sample',
                sourceObjectId: bank.key,
                targetObjectId: sample.key,
                candidateObjectIds: [],
                relationshipType: 'SBAC_SLOT_TO_SBNK',
                quality: 'KNOWN',
                basis: 'test',
                notes: [],
                assignmentIndex: 1,
                assignmentName: '',
                assignmentState: '',
                receiveChannelDisplay: '',
            },
            {
                id: 'sample-wave',
                sourceObjectId: sample.key,
                targetObjectId: wave.key,
                candidateObjectIds: [],
                relationshipType: 'SBNK_LEFT_MEMBER_TO_SMPL',
                quality: 'KNOWN',
                basis: 'test',
                notes: [],
                assignmentName: '',
                assignmentState: '',
                receiveChannelDisplay: '',
            },
        ];
        const opened = {
            sessionId: 17,
            companionDirectories: [],
            tree: [{ id: 'disk-17', name: 'DISK2', kind: 'disk' as const, childCount: 1, children: [volume] }],
            validation: {
                valid: true,
                issueCount: 0,
                errorCount: 0,
                warningCount: 0,
                objectCount: 3,
                relationshipCount: relationships.length,
            },
            objects: [],
            objectTotalCount: 0,
            initialVolume: volume,
            volumeMutationsAvailable: false,
            partitionMutationsAvailable: false,
            objectRenameAvailable: false,
            objectDeletionAvailable: false,
            waveDataCleanupAvailable: false,
            packageImportAvailable: false,
            packageExportAvailable: true,
        };
        mocks.sandboxDirectory.mockImplementation(async (directory) => ({
            directory,
            entries:
                directory.relativePath === ''
                    ? [{ name: 'DISK2', relativePath: 'DISK2', kind: 'DIRECTORY', size: null }]
                    : [],
            truncated: false,
            nextCursor: null,
        }));
        mocks.inspectSandboxMediaSource.mockResolvedValue('AXK_OBJECT_DIRECTORY');
        mocks.openImage.mockResolvedValueOnce(opened);
        mocks.objectPage.mockResolvedValue({
            objects: [bank, sample, wave],
            totalCount: 3,
        });
        mocks.relationshipPage.mockResolvedValue({ relationships, totalCount: relationships.length });
        mocks.attachCompanionDirectories.mockResolvedValue({
            ...opened,
            companionDirectories: [{ rootId: 'workspace', relativePath: 'DISK1' }],
        });
        let sequenceCompletion: ((result: unknown) => void) | undefined;
        const playSequence = vi
            .spyOn(AuditionController.prototype, 'playSequence')
            .mockImplementation((_sessionId, _objectIds, oncomplete) => {
                sequenceCompletion = oncomplete as (result: unknown) => void;
            });

        try {
            render(App);
            await fireEvent.click(screen.getByRole('button', { name: 'Open image' }));
            const picker = await screen.findByRole('dialog', { name: 'Open image' });
            await fireEvent.click(await within(picker).findByText('Yamaha'));
            await fireEvent.click(await within(picker).findByText('DISK2'));
            await fireEvent.click(within(picker).getByRole('button', { name: 'Open current folder' }));
            await fireEvent.click(screen.getByRole('button', { name: 'Sample Banks' }));
            await fireEvent.click(await screen.findByRole('button', { name: 'Play Slice Bank' }));

            sequenceCompletion?.({
                status: 'failed',
                playedCount: 0,
                skippedCount: 1,
                error: 'Wave Data continues on another sampler disk.',
                errorCode: 'companion_disks_required',
                failedObjectId: sample.key,
            });

            const dialog = await screen.findByRole('dialog', { name: 'Add companion disks' });
            await fireEvent.click(within(dialog).getByRole('button', { name: 'Search nearby folders and retry' }));
            await vi.waitFor(() =>
                expect(mocks.attachCompanionDirectories).toHaveBeenCalledWith(17, {
                    kind: 'immediate-siblings',
                }),
            );
            await vi.waitFor(() => expect(playSequence).toHaveBeenCalledTimes(2));
            expect(screen.queryByRole('dialog', { name: 'Add companion disks' })).toBeNull();
        } finally {
            playSequence.mockRestore();
        }
    });

    it('defaults the object browser to two-thirds of the middle workspace', () => {
        const { container } = render(App);

        expect(container.querySelector<HTMLElement>('.main-stage')?.style.getPropertyValue('--split-position')).toBe(
            '66.66666666666666%',
        );
    });

    it('keeps image management commands out of the top toolbar', async () => {
        render(App);

        expect(screen.queryByRole('button', { name: 'Save file' })).toBeNull();
        expect(screen.queryByRole('button', { name: 'Save directory' })).toBeNull();
        expect(screen.queryByRole('button', { name: 'Open disk image' })).toBeNull();
        expect(screen.queryByRole('button', { name: 'Storage locations' })).toBeNull();

        await fireEvent.click(screen.getByRole('button', { name: 'Image options' }));
        expect(screen.getByRole('menuitem', { name: 'Storage locations' })).toBeTruthy();
    });

    it('keeps image lifecycle commands in the image navigator', () => {
        render(App);

        const navigator = screen.getByRole('complementary', { name: 'Image navigator' });
        expect(screen.getByRole('button', { name: 'Open image' }).closest('aside')).toBe(navigator);
        expect(screen.getByRole('button', { name: 'Create image' }).closest('aside')).toBe(navigator);
        expect(screen.queryByRole('textbox', { name: 'Disk image path' })).toBeNull();
        expect(screen.queryByRole('button', { name: 'Eject image' })).toBeNull();
        expect(screen.getByText('Partitions, volumes and objects')).toBeTruthy();
    });

    it('closes the active image and returns to the initial empty state', async () => {
        render(App);

        await chooseNestedImage();
        await vi.waitFor(() => expect(mocks.openImage).toHaveBeenCalledOnce());
        await mocks.openImage.mock.results[0].value;
        await Promise.resolve();

        expect(screen.getAllByText('nested.hds')).not.toHaveLength(0);
        await fireEvent.click(screen.getByRole('button', { name: 'Eject image' }));

        await vi.waitFor(() => expect(mocks.closeImage).toHaveBeenCalledWith(17));
        await vi.waitFor(() => expect(screen.getByRole('button', { name: 'Open image' })).toBeTruthy());
        expect(screen.queryByRole('button', { name: 'Eject image' })).toBeNull();
    });

    it('preserves the active image when opening a replacement fails', async () => {
        render(App);

        await chooseNestedImage();
        await vi.waitFor(() => expect(mocks.openImage).toHaveBeenCalledOnce());
        await mocks.openImage.mock.results[0].value;
        mocks.openImage.mockRejectedValueOnce(new Error('Replacement is invalid'));

        await chooseNestedImage('Open another image');

        await vi.waitFor(() => expect(screen.getByText('Replacement is invalid')).toBeTruthy());
        expect(screen.getAllByText('nested.hds')).not.toHaveLength(0);
        expect(screen.getByRole('button', { name: 'Eject image' })).toBeTruthy();
        expect(mocks.closeImage).not.toHaveBeenCalledWith(17);
    });

    it('closes the active image session when the application is unmounted', async () => {
        const desktop = render(App);

        await chooseNestedImage();
        await vi.waitFor(() => expect(mocks.openImage).toHaveBeenCalledOnce());
        await mocks.openImage.mock.results[0].value;

        desktop.unmount();

        await vi.waitFor(() => expect(mocks.closeImage).toHaveBeenCalledWith(17));
    });

    it('reinspects, completes, and refreshes one object deletion before closing the dialog', async () => {
        const sample = {
            key: 'sample-1',
            objectType: 'SBNK',
            name: 'Piano C3',
            partitionIndex: 0,
            partitionName: 'Partition 0',
            volumeName: 'Piano',
            categoryName: 'SBNK',
            sfsId: 9,
            storedSizeBytes: 512,
            sampleRate: 0,
            rootKey: 60,
            frameCount: 0,
            sampleWidthBytes: 0,
        };
        const volume = {
            id: 'volume-1',
            name: 'Piano',
            kind: 'volume' as const,
            childCount: 0,
            partitionIndex: 0,
        };
        const opened = {
            sessionId: 17,
            tree: [{ id: 'disk-17', name: 'nested.hds', kind: 'disk' as const, childCount: 1, children: [volume] }],
            validation: {
                valid: true,
                issueCount: 0,
                errorCount: 0,
                warningCount: 0,
                objectCount: 1,
                relationshipCount: 0,
            },
            objects: [],
            objectTotalCount: 0,
            initialVolume: volume,
            volumeMutationsAvailable: true,
            partitionMutationsAvailable: true,
            objectDeletionAvailable: true,
            packageExportAvailable: true,
        };
        const inspection = {
            canApply: true,
            imageId: 'image-1',
            revision: 1,
            targetObjectIds: [sample.key],
            selectedObjectIds: [sample.key],
            impacts: [
                {
                    objectId: sample.key,
                    objectType: 'SBNK',
                    objectName: sample.name,
                    partitionIndex: 0,
                    partitionName: 'Partition 0',
                    volumeName: 'Piano',
                    role: 'TARGET',
                    status: 'REQUIRED',
                    selected: true,
                    storedSizeBytes: 512,
                    freedClusters: 1,
                    prerequisiteObjectIds: [],
                    reason: 'Selected object',
                },
                {
                    objectId: 'wave-1',
                    objectType: 'SMPL',
                    objectName: 'Piano C3',
                    partitionIndex: 0,
                    partitionName: 'Partition 0',
                    volumeName: 'Piano',
                    role: 'DEPENDENCY',
                    status: 'OPTIONAL',
                    selected: false,
                    storedSizeBytes: 4096,
                    freedClusters: 4,
                    prerequisiteObjectIds: [sample.key],
                    reason: 'Wave Data may be removed after its Sample is deleted',
                },
            ],
            references: [],
            blockers: [],
            warnings: [],
            estimatedFreedBytes: 1024,
            estimatedFreedClusters: 1,
        };
        const selectedInspection = {
            ...inspection,
            selectedObjectIds: [sample.key, 'wave-1'],
            impacts: inspection.impacts.map((impact) =>
                impact.objectId === 'wave-1' ? { ...impact, selected: true } : impact,
            ),
            estimatedFreedBytes: 5120,
            estimatedFreedClusters: 5,
        };
        mocks.openImage.mockResolvedValueOnce(opened);
        mocks.objectPage
            .mockResolvedValueOnce({ objects: [sample], totalCount: 1 })
            .mockResolvedValue({ objects: [], totalCount: 0 });
        mocks.inspectObjectDeletion.mockResolvedValueOnce(inspection).mockResolvedValue(selectedInspection);
        mocks.startObjectDeletion.mockResolvedValue({ jobId: 55, kind: 'images.delete', status: 'queued' });
        mocks.waitForJob.mockResolvedValue({
            jobId: 55,
            kind: 'images.delete',
            status: 'completed',
            result: { deletedObjectIds: [sample.key, 'wave-1'] },
        });
        let finishRefresh: ((value: Awaited<ReturnType<typeof mocks.refreshImage>>) => void) | undefined;
        mocks.refreshImage.mockReturnValue(
            new Promise((resolve) => {
                finishRefresh = resolve;
            }),
        );
        render(App);

        await chooseNestedImage();
        await fireEvent.click(screen.getByRole('button', { name: 'Samples' }));
        await vi.waitFor(() => expect(screen.getByText('Piano C3')).toBeTruthy());
        const row = screen.getByRole('button', { name: 'Inspect Piano C3' });
        await fireEvent.contextMenu(row);
        expect(screen.getByRole('button', { name: 'Export 1 selected object' })).toBeTruthy();
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Delete' }));
        await vi.waitFor(() => expect(screen.getByRole('dialog', { name: 'Delete Sample' })).toBeTruthy());
        await fireEvent.click(screen.getByRole('checkbox', { name: 'Also delete all (1)' }));
        await vi.waitFor(() => expect(mocks.inspectObjectDeletion).toHaveBeenCalledWith(17, [sample.key], ['wave-1']));
        await fireEvent.click(screen.getByRole('button', { name: 'Delete 2 objects' }));

        await vi.waitFor(() => expect(mocks.startObjectDeletion).toHaveBeenCalledOnce());
        expect(mocks.inspectObjectDeletion).toHaveBeenCalledTimes(3);
        expect(mocks.startObjectDeletion).toHaveBeenCalledWith(17, [sample.key], ['wave-1']);
        await vi.waitFor(() => expect(mocks.waitForJob).toHaveBeenCalledWith(55, expect.any(Function)));
        await vi.waitFor(() => expect(mocks.refreshImage).toHaveBeenCalledWith(17));
        expect(screen.getByRole('dialog', { name: 'Delete Sample' })).toBeTruthy();
        expect((screen.getByRole('button', { name: 'Deleting…' }) as HTMLButtonElement).disabled).toBe(true);
        finishRefresh?.({ ...opened, validation: { ...opened.validation, objectCount: 0 } });
        await vi.waitFor(() => expect(screen.queryByRole('dialog', { name: 'Delete Sample' })).toBeNull());
        expect(screen.queryByText('Piano C3')).toBeNull();
        expect(screen.queryByRole('button', { name: 'Export 1 selected object' })).toBeNull();
    });

    it('rediscovers and deletes only selected unreferenced Wave Data before refreshing', async () => {
        const volume = {
            id: 'volume-1',
            name: 'Piano',
            kind: 'volume' as const,
            childCount: 0,
            partitionIndex: 0,
        };
        const opened = {
            sessionId: 17,
            tree: [{ id: 'disk-17', name: 'nested.hds', kind: 'disk' as const, childCount: 1, children: [volume] }],
            validation: {
                valid: true,
                issueCount: 0,
                errorCount: 0,
                warningCount: 0,
                objectCount: 2,
                relationshipCount: 0,
            },
            objects: [],
            objectTotalCount: 0,
            initialVolume: volume,
            volumeMutationsAvailable: true,
            partitionMutationsAvailable: true,
            objectDeletionAvailable: true,
            waveDataCleanupAvailable: true,
        };
        const candidates = [
            {
                objectId: 'wave-unused-a',
                objectName: 'Unused A',
                objectType: 'SMPL' as const,
                partitionIndex: 0,
                partitionName: 'Partition 0',
                volumeName: volume.name,
                storedSizeBytes: 4096,
                recoverableBytes: 4096,
                recoverableClusters: 4,
            },
            {
                objectId: 'wave-unused-b',
                objectName: 'Unused B',
                objectType: 'SMPL' as const,
                partitionIndex: 0,
                partitionName: 'Partition 0',
                volumeName: volume.name,
                storedSizeBytes: 2048,
                recoverableBytes: 2048,
                recoverableClusters: 2,
            },
        ];
        const orphanInspection = {
            imageId: 'image-1',
            revision: 1,
            contentScopeId: volume.id,
            candidates,
            totalCandidateCount: candidates.length,
        };
        mocks.openImage.mockResolvedValueOnce(opened);
        mocks.refreshImage.mockResolvedValue({ ...opened, validation: { ...opened.validation, objectCount: 1 } });
        mocks.inspectWaveDataOrphans.mockResolvedValue(orphanInspection);
        mocks.inspectObjectDeletion.mockResolvedValue({
            canApply: true,
            imageId: 'image-1',
            revision: 1,
            targetObjectIds: ['wave-unused-a'],
            selectedObjectIds: ['wave-unused-a'],
            impacts: [
                {
                    objectId: 'wave-unused-a',
                    objectType: 'SMPL',
                    objectName: 'Unused A',
                    partitionIndex: 0,
                    partitionName: 'Partition 0',
                    volumeName: volume.name,
                    role: 'TARGET',
                    status: 'REQUIRED',
                    selected: true,
                    storedSizeBytes: 4096,
                    freedClusters: 4,
                    prerequisiteObjectIds: [],
                    reason: 'Requested deletion target',
                },
            ],
            references: [],
            blockers: [],
            warnings: [],
            estimatedFreedBytes: 4096,
            estimatedFreedClusters: 4,
        });
        mocks.startObjectDeletion.mockResolvedValue({ jobId: 56, kind: 'images.delete', status: 'queued' });
        mocks.waitForJob.mockResolvedValue({
            jobId: 56,
            kind: 'images.delete',
            status: 'completed',
            result: { deletedObjectIds: ['wave-unused-a'] },
        });
        render(App);

        await chooseNestedImage();
        await fireEvent.click(screen.getByRole('button', { name: 'Wave Data' }));
        await fireEvent.click(await screen.findByRole('button', { name: 'Clean up unreferenced Wave Data' }));
        const dialog = await screen.findByRole('dialog', { name: 'Clean up Wave Data' });
        const unusedA = within(dialog).getByRole('checkbox', {
            name: 'Delete Wave Data Unused A',
        }) as HTMLInputElement;
        const unusedB = within(dialog).getByRole('checkbox', {
            name: 'Delete Wave Data Unused B',
        }) as HTMLInputElement;
        expect(unusedA.checked).toBe(true);
        expect(unusedB.checked).toBe(true);
        await fireEvent.click(unusedB);
        await fireEvent.click(within(dialog).getByRole('button', { name: 'Delete 1 Wave Data object' }));

        await vi.waitFor(() => expect(mocks.inspectWaveDataOrphans).toHaveBeenCalledTimes(2));
        expect(mocks.inspectWaveDataOrphans).toHaveBeenNthCalledWith(1, 17, volume.id);
        expect(mocks.inspectWaveDataOrphans).toHaveBeenNthCalledWith(2, 17, volume.id);
        expect(mocks.inspectObjectDeletion).toHaveBeenCalledWith(17, ['wave-unused-a'], []);
        await vi.waitFor(() => expect(mocks.startObjectDeletion).toHaveBeenCalledOnce());
        expect(mocks.startObjectDeletion).toHaveBeenCalledWith(17, ['wave-unused-a'], []);
        await vi.waitFor(() => expect(mocks.refreshImage).toHaveBeenCalledWith(17));
        await vi.waitFor(() => expect(screen.queryByRole('dialog', { name: 'Clean up Wave Data' })).toBeNull());
    });

    it('renames an object, refreshes the image, and retains the selected object', async () => {
        const volume = {
            id: 'volume-1',
            name: 'Piano',
            kind: 'volume' as const,
            childCount: 0,
            partitionIndex: 0,
        };
        const waveData = {
            key: 'wave-1',
            objectType: 'SMPL',
            name: 'Old Wave',
            partitionIndex: 0,
            partitionName: 'Partition 0',
            volumeName: volume.name,
            categoryName: 'SMPL',
            sfsId: 9,
            storedSizeBytes: 4096,
            sampleRate: 44_100,
            rootKey: 60,
            frameCount: 44_100,
            sampleWidthBytes: 2,
        };
        const opened = {
            sessionId: 17,
            tree: [{ id: 'disk-17', name: 'nested.hds', kind: 'disk' as const, childCount: 1, children: [volume] }],
            validation: {
                valid: true,
                issueCount: 0,
                errorCount: 0,
                warningCount: 0,
                objectCount: 1,
                relationshipCount: 0,
            },
            objects: [],
            objectTotalCount: 0,
            initialVolume: volume,
            volumeMutationsAvailable: true,
            partitionMutationsAvailable: true,
            objectRenameAvailable: true,
            objectDeletionAvailable: true,
        };
        mocks.openImage.mockResolvedValueOnce(opened);
        mocks.refreshImage.mockResolvedValue(opened);
        mocks.objectPage
            .mockResolvedValueOnce({ objects: [waveData], totalCount: 1 })
            .mockResolvedValue({ objects: [{ ...waveData, name: 'New Wave' }], totalCount: 1 });
        mocks.startObjectRename.mockResolvedValue({ jobId: 56, kind: 'images.alter', status: 'queued' });
        mocks.waitForJob.mockResolvedValue({
            jobId: 56,
            kind: 'images.alter',
            status: 'completed',
            result: {},
        });
        render(App);

        await chooseNestedImage();
        await fireEvent.click(screen.getByRole('button', { name: 'Wave Data' }));
        const row = await screen.findByRole('button', { name: 'Inspect Old Wave' });
        await fireEvent.click(row);
        await fireEvent.contextMenu(row);
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Rename' }));
        const dialog = await screen.findByRole('dialog', { name: 'Rename Wave Data' });
        await fireEvent.input(within(dialog).getByLabelText('Wave Data name'), {
            target: { value: 'New Wave' },
        });
        await fireEvent.click(within(dialog).getByRole('button', { name: 'Rename' }));

        await vi.waitFor(() =>
            expect(mocks.startObjectRename).toHaveBeenCalledWith(17, {
                kind: 'wave-data',
                partitionIndex: 0,
                volumeName: 'Piano',
                waveformName: 'Old Wave',
                newWaveformName: 'New Wave',
            }),
        );
        await vi.waitFor(() => expect(mocks.refreshImage).toHaveBeenCalledWith(17));
        await vi.waitFor(() => expect(screen.queryByRole('dialog', { name: 'Rename Wave Data' })).toBeNull());
        expect(
            screen
                .getByRole('button', { name: 'Inspect New Wave' })
                .closest('.wave-data-row')
                ?.classList.contains('active'),
        ).toBe(true);
        expect(screen.getByRole('complementary', { name: 'Object inspector' }).textContent).toContain('New Wave');
    });

    it('closes an image session that finishes opening after the application is unmounted', async () => {
        let finishOpening: ((value: Awaited<ReturnType<typeof mocks.openImage>>) => void) | undefined;
        mocks.openImage.mockReturnValueOnce(
            new Promise((resolve) => {
                finishOpening = resolve;
            }),
        );
        const desktop = render(App);

        await chooseNestedImage();
        await vi.waitFor(() => expect(mocks.openImage).toHaveBeenCalledOnce());
        desktop.unmount();

        finishOpening?.({
            sessionId: 29,
            tree: [{ id: 'disk-29', name: 'nested.hds', kind: 'disk', childCount: 0 }],
            validation: {
                valid: true,
                issueCount: 0,
                errorCount: 0,
                warningCount: 0,
                objectCount: 0,
                relationshipCount: 0,
            },
            objects: [],
            objectTotalCount: 0,
            initialVolume: null,
            volumeMutationsAvailable: true,
            partitionMutationsAvailable: true,
            objectDeletionAvailable: true,
        });

        await vi.waitFor(() => expect(mocks.closeImage).toHaveBeenCalledWith(29));
    });

    it('keeps open-image selection free of destructive file-management commands', async () => {
        render(App);

        await fireEvent.click(screen.getByRole('button', { name: 'Open image' }));
        await fireEvent.click(await screen.findByText('Yamaha'));
        await fireEvent.click(await screen.findByText('images'));
        expect(screen.queryByRole('button', { name: /More actions/ })).toBeNull();
        expect(screen.queryByRole('menuitem', { name: 'Delete' })).toBeNull();
        expect(screen.queryByRole('button', { name: 'New folder' })).toBeNull();
    });

    it('continues from package verification into import planning without proxy identity checks', async () => {
        const volume = {
            id: 'volume-1',
            name: 'My Volume',
            kind: 'volume' as const,
            childCount: 0,
            partitionIndex: 0,
        };
        mocks.openImage.mockResolvedValueOnce({
            sessionId: 17,
            tree: [{ id: 'disk-17', name: 'nested.hds', kind: 'disk', childCount: 1, children: [volume] }],
            validation: {
                valid: true,
                issueCount: 0,
                errorCount: 0,
                warningCount: 0,
                objectCount: 0,
                relationshipCount: 0,
            },
            objects: [],
            objectTotalCount: 0,
            initialVolume: volume,
            volumeMutationsAvailable: true,
            partitionMutationsAvailable: true,
            objectDeletionAvailable: true,
            packageImportAvailable: true,
            packageExportAvailable: true,
        });
        mocks.inspectPackage.mockResolvedValue({
            schemaVersion: '1.0',
            packageId: 'package-1',
            packageKind: 'VOLUME',
            requiredExtension: '.axkvol',
            sourceMediaKind: 'SFS',
            valid: true,
            payloadsVerified: true,
            roots: [{ kind: 'VOLUME', displayName: 'Grand Piano', nodeIds: [] }],
            objects: [],
            relationships: [],
            relationshipCount: 0,
            issues: [],
        });
        mocks.planImagePackageImport.mockResolvedValue({
            schemaVersion: '1.0',
            imageId: 'image-1',
            revision: 1,
            planToken: 'plan-1',
            expiresInSeconds: 600,
            planId: 'plan-id',
            targetKind: 'SFS',
            targetSnapshotId: 'snapshot-1',
            valid: true,
            warnings: [],
            conflicts: [],
            actions: [],
            allocation: [],
        });
        render(App);

        await chooseNestedImage();
        await vi.waitFor(() => expect(screen.getByRole('button', { name: /My Volume/ })).toBeTruthy());
        mocks.sandboxDirectory.mockResolvedValue({
            directory: { rootId: 'workspace', relativePath: '' },
            entries: [
                {
                    name: 'GrPiano Fazioli.axkvol',
                    relativePath: 'GrPiano Fazioli.axkvol',
                    kind: 'FILE',
                    size: 33832158,
                },
            ],
            truncated: false,
            nextCursor: null,
        });
        await fireEvent.contextMenu(screen.getByRole('button', { name: /My Volume/ }));
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Import package…' }));
        await fireEvent.click(screen.getByRole('button', { name: /Storage location/ }));
        const picker = await screen.findByRole('dialog', { name: 'Choose axklib package' });
        expect(screen.queryByRole('dialog', { name: 'Import axklib package' })).toBeNull();
        expect(screen.getAllByRole('dialog')).toHaveLength(1);
        await fireEvent.click(await within(picker).findByText('Yamaha'));
        await fireEvent.click(await within(picker).findByText('GrPiano Fazioli.axkvol'));

        await vi.waitFor(() =>
            expect(mocks.planImagePackageImport).toHaveBeenCalledWith(
                17,
                expect.objectContaining({
                    kind: 'server-file',
                    reference: { rootId: 'workspace', relativePath: 'GrPiano Fazioli.axkvol' },
                }),
                0,
                'My Volume',
                [],
            ),
        );
        expect(await screen.findByText('Ready to import')).toBeTruthy();
        expect(screen.queryByText(/Uploading package/)).toBeNull();
    });

    it('keeps one mixed package export selection across tabs and volumes', async () => {
        const pianoVolume = {
            id: 'volume-piano',
            name: 'Piano',
            kind: 'volume' as const,
            childCount: 0,
            partitionIndex: 0,
        };
        const drumsVolume = {
            id: 'volume-drums',
            name: 'Drums',
            kind: 'volume' as const,
            childCount: 0,
            partitionIndex: 1,
        };
        const program = {
            key: 'program-piano',
            objectType: 'PROG',
            name: '001: Concert Grand',
            partitionIndex: 0,
            partitionName: 'Partition 0',
            volumeName: 'Piano',
            categoryName: 'PROG',
            sfsId: 1,
            storedSizeBytes: 512,
        };
        const bank = {
            key: 'bank-drums',
            objectType: 'SBAC',
            name: 'Studio Kit',
            partitionIndex: 1,
            partitionName: 'Partition 1',
            volumeName: 'Drums',
            categoryName: 'SBAC',
            sfsId: 2,
            storedSizeBytes: 512,
        };
        mocks.openImage.mockResolvedValueOnce({
            sessionId: 17,
            tree: [
                {
                    id: 'disk-17',
                    name: 'nested.hds',
                    kind: 'disk',
                    childCount: 2,
                    children: [pianoVolume, drumsVolume],
                },
            ],
            validation: {
                valid: true,
                issueCount: 0,
                errorCount: 0,
                warningCount: 0,
                objectCount: 2,
                relationshipCount: 0,
            },
            objects: [],
            objectTotalCount: 0,
            initialVolume: pianoVolume,
            volumeMutationsAvailable: true,
            partitionMutationsAvailable: true,
            objectDeletionAvailable: true,
            packageImportAvailable: true,
            packageExportAvailable: true,
        });
        mocks.objectPage.mockImplementation(async (_sessionId, _offset, _limit, filter) => ({
            objects: filter?.scopeId === pianoVolume.id ? [program] : filter?.scopeId === drumsVolume.id ? [bank] : [],
            totalCount: 1,
        }));
        render(App);

        await chooseNestedImage();
        await fireEvent.click(await screen.findByText('Concert Grand'));
        expect(screen.getByRole('status').textContent).toContain('1 selected');

        await fireEvent.click(screen.getByRole('button', { name: /Drums/ }));
        await fireEvent.click(screen.getByRole('button', { name: 'Sample Banks' }));
        await fireEvent.click(await screen.findByRole('button', { name: 'Inspect Studio Kit' }), { ctrlKey: true });

        expect(screen.getByRole('status').textContent).toContain('2 selected');
        await fireEvent.click(screen.getByRole('button', { name: 'Export 2 selected objects' }));

        const dialog = screen.getByRole('dialog', { name: 'Export axklib package' });
        expect(within(dialog).getByText('Partition 0 · Piano')).toBeTruthy();
        expect(within(dialog).getByText('Partition 1 · Drums')).toBeTruthy();
        expect(within(dialog).getByText('1 Program · 1 Sample Bank')).toBeTruthy();
        await fireEvent.click(within(dialog).getByRole('button', { name: /Storage location/ }));
        const picker = await screen.findByRole('navigation', { name: 'Storage location' });
        expect(screen.queryByRole('heading', { name: 'Export package' })).toBeNull();
        expect(screen.getAllByRole('dialog')).toHaveLength(1);
        await fireEvent.click(within(picker.closest('[role="dialog"]')!).getByRole('button', { name: 'Cancel' }));
        expect(await screen.findByRole('heading', { name: 'Export package' })).toBeTruthy();
    });

    it('opens volume package export from a read-only AXK object directory', async () => {
        const objectDirectoryVolume = {
            id: 'object-directory-volume',
            name: 'Object directory',
            kind: 'volume' as const,
            childCount: 0,
            partitionIndex: 0,
        };
        mocks.openImage.mockResolvedValueOnce({
            sessionId: 17,
            tree: [
                {
                    id: 'disk-17',
                    name: 'FS1R',
                    kind: 'disk',
                    childCount: 1,
                    children: [objectDirectoryVolume],
                },
            ],
            validation: {
                valid: true,
                issueCount: 0,
                errorCount: 0,
                warningCount: 0,
                objectCount: 1,
                relationshipCount: 0,
            },
            objects: [],
            objectTotalCount: 0,
            initialVolume: objectDirectoryVolume,
            volumeMutationsAvailable: false,
            partitionMutationsAvailable: false,
            objectRenameAvailable: false,
            objectDeletionAvailable: false,
            waveDataCleanupAvailable: false,
            packageImportAvailable: false,
            packageExportAvailable: true,
        });
        render(App);

        await chooseNestedImage();
        await fireEvent.contextMenu(await screen.findByRole('button', { name: /Object directory/ }));
        expect(screen.queryByRole('menuitem', { name: 'Import package…' })).toBeNull();
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Export package…' }));

        const dialog = screen.getByRole('dialog', { name: 'Export axklib package' });
        expect(within(dialog).getByText('Export “Object directory”')).toBeTruthy();
        expect(within(dialog).getByRole('button', { name: /Storage location/ })).toBeTruthy();
    });

    it('suppresses context menus only in the desktop runtime', async () => {
        const runtime = window as unknown as { __TAURI_INTERNALS__?: unknown };
        runtime.__TAURI_INTERNALS__ = {};
        const desktop = render(App);
        const desktopEvent = new MouseEvent('contextmenu', { bubbles: true, cancelable: true });
        window.dispatchEvent(desktopEvent);
        expect(desktopEvent.defaultPrevented).toBe(true);
        desktop.unmount();

        delete runtime.__TAURI_INTERNALS__;
        render(App);
        const browserEvent = new MouseEvent('contextmenu', { bubbles: true, cancelable: true });
        window.dispatchEvent(browserEvent);
        expect(browserEvent.defaultPrevented).toBe(false);
    });

    it('prevents WebKit URI-list file drops from navigating away', async () => {
        render(App);

        const file = new File(['audio'], 'take.wav', { type: 'audio/wav' });
        const dataTransfer = {
            types: ['text/uri-list'],
            files: [file],
            dropEffect: 'none',
        } as unknown as DataTransfer;
        const dragOver = new Event('dragover', { bubbles: true, cancelable: true }) as DragEvent;
        Object.defineProperty(dragOver, 'dataTransfer', { value: dataTransfer });
        window.dispatchEvent(dragOver);
        expect(dragOver.defaultPrevented).toBe(true);

        const drop = new Event('drop', { bubbles: true, cancelable: true }) as DragEvent;
        Object.defineProperty(drop, 'dataTransfer', { value: dataTransfer });
        window.dispatchEvent(drop);
        expect(drop.defaultPrevented).toBe(true);
        const dialog = await screen.findByRole('dialog', { name: 'Audio import unavailable' });
        expect(
            within(dialog).getByText('Select a writable volume in Contents, then drop the audio files again.'),
        ).toBeTruthy();
    });

    it('routes native Tauri file drops through the audio import flow', async () => {
        const runtime = window as unknown as { __TAURI_INTERNALS__?: unknown };
        runtime.__TAURI_INTERNALS__ = {};
        render(App);

        await vi.waitFor(() => expect(mocks.listenForNativeMediaDrops).toHaveBeenCalledOnce());
        const callbacks = mocks.listenForNativeMediaDrops.mock.calls[0][0];
        callbacks.onDrop([new File(['audio'], 'take.wav', { type: 'audio/wav' })], { x: 20, y: 30 }, 1);

        const dialog = await screen.findByRole('dialog', { name: 'Audio import unavailable' });
        expect(
            within(dialog).getByText('Select a writable volume in Contents, then drop the audio files again.'),
        ).toBeTruthy();
        delete runtime.__TAURI_INTERNALS__;
    });

    it('routes browser MIDI drops through the Sequence import review on the Sequences tab', async () => {
        const volume = {
            id: 'volume-1',
            name: 'My Volume',
            kind: 'volume' as const,
            childCount: 0,
            partitionIndex: 0,
        };
        mocks.openImage.mockResolvedValueOnce(openedImageWithVolumes([volume], volume));
        mocks.uploadClientFile.mockResolvedValue({
            kind: 'client-upload',
            reference: { uploadId: 'midi-upload' },
            uploadKind: 'MIDI',
            displayName: 'intro.mid',
        });
        render(App);
        await chooseNestedImage();
        await fireEvent.click(screen.getByRole('button', { name: 'Sequences' }));

        const dataTransfer = {
            types: ['Files'],
            files: [new File(['midi'], 'intro.MID', { type: 'audio/midi' })],
            dropEffect: 'none',
        } as unknown as DataTransfer;
        const drop = new Event('drop', { bubbles: true, cancelable: true }) as DragEvent;
        Object.defineProperty(drop, 'dataTransfer', { value: dataTransfer });
        window.dispatchEvent(drop);

        const dialog = await screen.findByRole('dialog', { name: 'Import MIDI' });
        expect(within(dialog).getByText('Volume My Volume')).toBeTruthy();
        expect(within(dialog).getByDisplayValue('intro')).toBeTruthy();
        await vi.waitFor(() =>
            expect(mocks.uploadClientFile).toHaveBeenCalledWith(
                expect.objectContaining({ name: 'intro.MID', type: 'audio/midi' }),
                'MIDI',
                expect.any(Function),
                expect.any(AbortSignal),
            ),
        );
    });

    it('routes native MIDI drops through the same Sequence import review', async () => {
        const runtime = window as unknown as { __TAURI_INTERNALS__?: unknown };
        runtime.__TAURI_INTERNALS__ = {};
        const volume = {
            id: 'volume-1',
            name: 'My Volume',
            kind: 'volume' as const,
            childCount: 0,
            partitionIndex: 0,
        };
        mocks.openImage.mockResolvedValueOnce(openedImageWithVolumes([volume], volume));
        mocks.uploadClientFile.mockResolvedValue({
            kind: 'client-upload',
            reference: { uploadId: 'midi-upload' },
            uploadKind: 'MIDI',
            displayName: 'native.mid',
        });
        render(App);
        await chooseNestedImage();
        await fireEvent.click(screen.getByRole('button', { name: 'Sequences' }));
        await vi.waitFor(() => expect(mocks.listenForNativeMediaDrops).toHaveBeenCalledOnce());

        const callbacks = mocks.listenForNativeMediaDrops.mock.calls[0][0];
        callbacks.onDrop(
            [
                {
                    name: 'native.mid',
                    type: 'audio/midi',
                    size: 4,
                    readChunk: async () => new Blob(['midi']),
                },
            ],
            { x: 20, y: 30 },
            1,
        );

        const dialog = await screen.findByRole('dialog', { name: 'Import MIDI' });
        expect(within(dialog).getByDisplayValue('native')).toBeTruthy();
        delete runtime.__TAURI_INTERNALS__;
    });

    it('requires the Sequences tab for MIDI drops and rejects mixed media drops', async () => {
        render(App);

        const midiTransfer = {
            types: ['Files'],
            files: [new File(['midi'], 'song.mid', { type: 'audio/midi' })],
            dropEffect: 'none',
        } as unknown as DataTransfer;
        const midiDrop = new Event('drop', { bubbles: true, cancelable: true }) as DragEvent;
        Object.defineProperty(midiDrop, 'dataTransfer', { value: midiTransfer });
        window.dispatchEvent(midiDrop);
        let unavailable = await screen.findByRole('dialog', { name: 'MIDI import unavailable' });
        expect(
            within(unavailable).getByText(
                'Open the Sequences tab, select a writable volume, then drop the MIDI files again.',
            ),
        ).toBeTruthy();
        expect(screen.queryByRole('dialog', { name: 'Import MIDI' })).toBeNull();
        await fireEvent.click(within(unavailable).getByRole('button', { name: 'OK' }));

        await fireEvent.click(screen.getByRole('button', { name: 'Sequences' }));
        const mixedTransfer = {
            types: ['Files'],
            files: [
                new File(['audio'], 'take.wav', { type: 'audio/wav' }),
                new File(['midi'], 'song.mid', { type: 'audio/midi' }),
            ],
            dropEffect: 'none',
        } as unknown as DataTransfer;
        const mixedDrop = new Event('drop', { bubbles: true, cancelable: true }) as DragEvent;
        Object.defineProperty(mixedDrop, 'dataTransfer', { value: mixedTransfer });
        window.dispatchEvent(mixedDrop);
        unavailable = await screen.findByRole('dialog', { name: 'Import unavailable' });
        expect(within(unavailable).getByText('Drop audio and MIDI files separately.')).toBeTruthy();
        expect(screen.queryByRole('dialog', { name: 'Import MIDI' })).toBeNull();
        expect(screen.queryByRole('dialog', { name: 'Import audio' })).toBeNull();
    });

    it('switches to a dropped-on volume before opening MIDI import review', async () => {
        const firstVolume = {
            id: 'volume-1',
            name: 'First Volume',
            kind: 'volume' as const,
            childCount: 0,
            partitionIndex: 0,
        };
        const secondVolume = {
            id: 'volume-2',
            name: 'Second Volume',
            kind: 'volume' as const,
            childCount: 0,
            partitionIndex: 0,
        };
        mocks.openImage.mockResolvedValueOnce(openedImageWithVolumes([firstVolume, secondVolume], firstVolume));
        mocks.uploadClientFile.mockResolvedValue({
            kind: 'client-upload',
            reference: { uploadId: 'midi-upload' },
            uploadKind: 'MIDI',
            displayName: 'target.mid',
        });
        render(App);
        await chooseNestedImage();
        await fireEvent.click(screen.getByRole('button', { name: 'Sequences' }));

        const dataTransfer = {
            types: ['Files'],
            files: [new File(['midi'], 'target.mid', { type: 'audio/midi' })],
            dropEffect: 'none',
        } as unknown as DataTransfer;
        const drop = new Event('drop', { bubbles: true, cancelable: true }) as DragEvent;
        Object.defineProperty(drop, 'dataTransfer', { value: dataTransfer });
        screen.getByText('Second Volume').dispatchEvent(drop);

        const dialog = await screen.findByRole('dialog', { name: 'Import MIDI' });
        expect(within(dialog).getByText('Volume Second Volume')).toBeTruthy();
        expect(mocks.objectPage).toHaveBeenCalledWith(17, 0, 256, { scopeId: 'volume-2' });
    });

    it('selects several workspace audio files and inspects them without uploading', async () => {
        const volume = {
            id: 'volume-1',
            name: 'My Volume',
            kind: 'volume' as const,
            childCount: 0,
            partitionIndex: 0,
        };
        mocks.openImage.mockResolvedValueOnce({
            sessionId: 17,
            tree: [{ id: 'disk-17', name: 'nested.hds', kind: 'disk', childCount: 1, children: [volume] }],
            validation: {
                valid: true,
                issueCount: 0,
                errorCount: 0,
                warningCount: 0,
                objectCount: 0,
                relationshipCount: 0,
            },
            objects: [],
            objectTotalCount: 0,
            initialVolume: volume,
            volumeMutationsAvailable: true,
            partitionMutationsAvailable: true,
            objectRenameAvailable: true,
            objectDeletionAvailable: true,
            waveDataCleanupAvailable: false,
        });
        render(App);
        await chooseNestedImage();
        await fireEvent.click(screen.getByRole('button', { name: 'Samples' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Import audio' }));

        const sourceDialog = await screen.findByRole('dialog', { name: 'Import audio' });
        await fireEvent.click(within(sourceDialog).getByRole('button', { name: /Storage location/ }));
        mocks.sandboxDirectory.mockResolvedValue({
            directory: { rootId: 'workspace', relativePath: '' },
            entries: [
                { name: 'kick.wav', relativePath: 'kick.wav', kind: 'FILE', size: 1024 },
                { name: 'snare.FLAC', relativePath: 'snare.FLAC', kind: 'FILE', size: 2048 },
                { name: 'notes.txt', relativePath: 'notes.txt', kind: 'FILE', size: 32 },
            ],
            truncated: false,
            nextCursor: null,
        });
        let picker = await screen.findByRole('dialog', { name: 'Choose audio files' });
        expect(screen.queryByRole('dialog', { name: 'Import audio' })).toBeNull();
        expect(screen.getAllByRole('dialog')).toHaveLength(1);
        await fireEvent.click(within(picker).getByRole('button', { name: 'Cancel' }));
        const restoredSourceDialog = await screen.findByRole('dialog', { name: 'Import audio' });
        await fireEvent.click(within(restoredSourceDialog).getByRole('button', { name: /Storage location/ }));
        picker = await screen.findByRole('dialog', { name: 'Choose audio files' });
        await fireEvent.click(await within(picker).findByText('Yamaha'));
        await fireEvent.click(await within(picker).findByText('kick.wav'));
        await fireEvent.click(await within(picker).findByText('snare.FLAC'));
        expect(within(picker).queryByText('notes.txt')).toBeNull();
        await fireEvent.click(within(picker).getByRole('button', { name: 'Select 2 files' }));

        await vi.waitFor(() =>
            expect(mocks.inspectAudio).toHaveBeenCalledWith(
                expect.objectContaining({
                    kind: 'server-file',
                    reference: { rootId: 'workspace', relativePath: 'kick.wav' },
                }),
            ),
        );
        expect(mocks.inspectAudio).toHaveBeenCalledWith(
            expect.objectContaining({
                kind: 'server-file',
                reference: { rootId: 'workspace', relativePath: 'snare.FLAC' },
            }),
        );
        expect(mocks.uploadClientFile).not.toHaveBeenCalled();
        expect(await screen.findAllByDisplayValue('kick')).toHaveLength(2);
        expect(await screen.findAllByDisplayValue('snare')).toHaveLength(2);
    });

    it('normalizes browser-specific audio MIME types before local upload', async () => {
        const volume = {
            id: 'volume-1',
            name: 'My Volume',
            kind: 'volume' as const,
            childCount: 0,
            partitionIndex: 0,
        };
        mocks.openImage.mockResolvedValueOnce({
            sessionId: 17,
            tree: [{ id: 'disk-17', name: 'nested.hds', kind: 'disk', childCount: 1, children: [volume] }],
            validation: {
                valid: true,
                issueCount: 0,
                errorCount: 0,
                warningCount: 0,
                objectCount: 0,
                relationshipCount: 0,
            },
            objects: [],
            objectTotalCount: 0,
            initialVolume: volume,
            volumeMutationsAvailable: true,
            partitionMutationsAvailable: true,
            objectRenameAvailable: true,
            objectDeletionAvailable: true,
            waveDataCleanupAvailable: false,
        });
        mocks.uploadClientFile.mockResolvedValue({
            kind: 'client-upload',
            reference: { uploadId: 'audio-upload' },
            uploadKind: 'AUDIO',
            displayName: '16bit_11k.wav',
        });
        const { container } = render(App);
        await chooseNestedImage();
        await fireEvent.click(screen.getByRole('button', { name: 'Samples' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Import audio' }));
        await fireEvent.click(screen.getByRole('button', { name: /This computer/ }));

        const input = container.querySelector<HTMLInputElement>('input[type="file"]');
        expect(input).toBeTruthy();
        const file = new File(['audio'], '16bit_11k.wav', { type: 'audio/vnd.wave' });
        await fireEvent.change(input!, { target: { files: [file] } });

        await vi.waitFor(() => expect(mocks.uploadClientFile).toHaveBeenCalled());
        expect(mocks.uploadClientFile.mock.calls[0][0]).toMatchObject({
            name: '16bit_11k.wav',
            type: 'audio/wav',
        });
    });

    it('consumes unsupported and empty file drops without opening the import dialog', async () => {
        render(App);

        const unsupportedTransfer = {
            types: ['text/uri-list'],
            files: [new File(['text'], 'notes.txt', { type: 'text/plain' })],
            dropEffect: 'none',
        } as unknown as DataTransfer;
        const unsupportedDrop = new Event('drop', { bubbles: true, cancelable: true }) as DragEvent;
        Object.defineProperty(unsupportedDrop, 'dataTransfer', { value: unsupportedTransfer });
        window.dispatchEvent(unsupportedDrop);
        expect(unsupportedDrop.defaultPrevented).toBe(true);
        expect(await screen.findByText('No supported WAV, FLAC, or AIFF files were selected')).toBeTruthy();
        expect(screen.queryByRole('dialog', { name: 'Import audio' })).toBeNull();

        const emptyTransfer = {
            types: ['text/uri-list'],
            files: [],
            dropEffect: 'none',
        } as unknown as DataTransfer;
        const emptyDrop = new Event('drop', { bubbles: true, cancelable: true }) as DragEvent;
        Object.defineProperty(emptyDrop, 'dataTransfer', { value: emptyTransfer });
        window.dispatchEvent(emptyDrop);
        expect(emptyDrop.defaultPrevented).toBe(true);
        expect(screen.queryByRole('dialog', { name: 'Import audio' })).toBeNull();
    });

    it('restores the last image-picker directory after cancelling', async () => {
        render(App);

        await fireEvent.click(screen.getByRole('button', { name: 'Open image' }));
        await fireEvent.click(await screen.findByText('Yamaha'));
        await fireEvent.click(await screen.findByText('images'));
        expect(await screen.findByText('nested.hds')).toBeTruthy();
        await fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));

        await fireEvent.click(screen.getByRole('button', { name: 'Open image' }));
        expect(await screen.findByText('nested.hds')).toBeTruthy();
        expect(mocks.sandboxDirectory).toHaveBeenLastCalledWith({ rootId: 'workspace', relativePath: 'images' });
    });

    it('starts hard-disk image creation through a dedicated destination picker', async () => {
        render(App);

        await fireEvent.click(screen.getByRole('button', { name: 'Create image' }));
        await fireEvent.click(await screen.findByText('Yamaha'));
        await fireEvent.click(await screen.findByText('images'));
        await fireEvent.click(screen.getByRole('button', { name: 'Select directory' }));

        const dialog = await screen.findByRole('dialog', { name: 'Create HD image' });
        expect(dialog.querySelector('output')?.textContent).toBe('Yamaha/images');
    });
});

function openedImageWithVolumes(
    volumes: {
        id: string;
        name: string;
        kind: 'volume';
        childCount: number;
        partitionIndex: number;
    }[],
    initialVolume: (typeof volumes)[number],
) {
    return {
        sessionId: 17,
        companionDirectories: [],
        tree: [
            { id: 'disk-17', name: 'nested.hds', kind: 'disk' as const, childCount: volumes.length, children: volumes },
        ],
        validation: {
            valid: true,
            issueCount: 0,
            errorCount: 0,
            warningCount: 0,
            objectCount: 0,
            relationshipCount: 0,
        },
        objects: [],
        objectTotalCount: 0,
        initialVolume,
        volumeMutationsAvailable: true,
        partitionMutationsAvailable: true,
        objectRenameAvailable: true,
        objectDeletionAvailable: true,
        waveDataCleanupAvailable: false,
    };
}
