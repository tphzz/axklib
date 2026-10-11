import { describe, expect, it, vi } from 'vitest';
import { ImageSessionController } from './actions';
import type { ImageLocation } from '../../lib/storageLocations';
import type { OpenedImage } from '../../lib/transport';

const firstLocation: ImageLocation = {
    kind: 'server-file',
    reference: { rootId: 'root', relativePath: 'first.hds' },
    displayName: 'first.hds',
};
const secondLocation: ImageLocation = {
    kind: 'server-file',
    reference: { rootId: 'root', relativePath: 'second.hds' },
    displayName: 'second.hds',
};

function opened(sessionId: number): OpenedImage {
    return {
        sessionId,
        allocationInspectionAvailable: false,
        revision: 1,
        companionSources: [],
        floppySet: null,
        tree: [],
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
        volumeMutationsAvailable: false,
        partitionMutationsAvailable: false,
        objectRenameAvailable: false,
        objectDeletionAvailable: false,
        waveDataCleanupAvailable: false,
        programGenerationAvailable: false,
        programAssignmentCleanupAvailable: false,
        packageImportAvailable: false,
        packageExportAvailable: false,
        volumePackageExportAvailable: false,
        volumeFloppyExportAvailable: false,
        audioExportAvailable: false,
        sequenceExportAvailable: false,
        mediaConversionAvailable: false,
        extentLayoutRepairAvailable: false,
    };
}

describe('ImageSessionController', () => {
    it('prepares and refreshes a candidate before replacing the previous workspace', async () => {
        const closeImage = vi.fn(async () => undefined);
        const refreshImage = vi.fn(async () => ({ ...opened(2), objectRenameAvailable: true }));
        const controller = new ImageSessionController(
            {
                openImage: vi.fn().mockResolvedValueOnce(opened(1)).mockResolvedValueOnce(opened(2)),
                keepImageAlive: vi.fn(),
                refreshImage,
                closeImage,
            },
            async () => undefined,
            () => undefined,
        );
        await controller.open(firstLocation);
        const prepare = vi.fn(async (candidate: OpenedImage) => {
            expect(candidate.sessionId).toBe(2);
            expect(controller.snapshot().sessionId).toBe(1);
            expect(closeImage).not.toHaveBeenCalled();
        });
        await expect(controller.open(secondLocation, undefined, prepare)).resolves.toMatchObject({
            sessionId: 2,
            objectRenameAvailable: true,
        });
        expect(refreshImage).toHaveBeenCalledWith(2);
        expect(closeImage).toHaveBeenCalledWith(1);
        expect(controller.snapshot().sessionId).toBe(2);
    });

    it('leaves the current session intact when candidate preparation or refresh fails', async () => {
        const closeImage = vi.fn(async () => undefined);
        const controller = new ImageSessionController(
            {
                openImage: vi.fn().mockResolvedValueOnce(opened(1)).mockResolvedValue(opened(2)),
                keepImageAlive: vi.fn(),
                refreshImage: vi.fn(async () => {
                    throw new Error('Refresh failed');
                }),
                closeImage,
            },
            async () => undefined,
            () => undefined,
        );
        await controller.open(firstLocation);
        await expect(
            controller.open(secondLocation, undefined, async () => {
                throw new Error('Permission declined');
            }),
        ).rejects.toThrow('Permission declined');
        expect(controller.snapshot()).toMatchObject({ sessionId: 1, location: firstLocation, opening: false });
        expect(closeImage).toHaveBeenCalledWith(2);
        expect(closeImage).not.toHaveBeenCalledWith(1);
        await expect(controller.open(secondLocation, undefined, async () => {})).rejects.toThrow('Refresh failed');
        expect(controller.snapshot().sessionId).toBe(1);
    });

    it('does not adopt a candidate cancelled while authorization is pending', async () => {
        let finish!: () => void;
        const closeImage = vi.fn(async () => undefined);
        const refreshImage = vi.fn(async () => opened(2));
        const controller = new ImageSessionController(
            {
                openImage: vi.fn().mockResolvedValueOnce(opened(1)).mockResolvedValueOnce(opened(2)),
                keepImageAlive: vi.fn(),
                refreshImage,
                closeImage,
            },
            async () => undefined,
            () => undefined,
        );
        await controller.open(firstLocation);
        const prepare = vi.fn(() => new Promise<void>((resolve) => (finish = resolve)));
        const opening = controller.open(secondLocation, undefined, prepare);
        await vi.waitFor(() => expect(prepare).toHaveBeenCalledOnce());
        controller.cancelOpen();
        finish();
        await expect(opening).rejects.toMatchObject({ name: 'AbortError' });
        expect(controller.snapshot().sessionId).toBe(1);
        expect(refreshImage).not.toHaveBeenCalled();
        expect(closeImage).toHaveBeenCalledWith(2);
    });

    it('retains failed staged cleanup so it can be retried without another authorization', async () => {
        const closeImage = vi.fn(async (): Promise<void> => {
            throw new Error('Cleanup failed');
        });
        const controller = new ImageSessionController(
            { openImage: vi.fn(async () => opened(2)), keepImageAlive: vi.fn(), refreshImage: vi.fn(), closeImage },
            async () => undefined,
            () => undefined,
        );
        const prepare = vi.fn(async () => {
            throw new Error('Denied');
        });
        await expect(controller.open(secondLocation, undefined, prepare)).rejects.toThrow('Cleanup failed');
        closeImage.mockResolvedValue(undefined);
        await controller.cleanupOpen();
        expect(closeImage).toHaveBeenCalledTimes(2);
        expect(prepare).toHaveBeenCalledOnce();
        expect(controller.snapshot().sessionId).toBeNull();
    });

    it('keeps only the active session alive', async () => {
        const keepImageAlive = vi.fn(async () => undefined);
        const controller = new ImageSessionController(
            {
                openImage: vi.fn(async () => opened(7)),
                keepImageAlive,
                refreshImage: vi.fn(),
                closeImage: vi.fn(),
            },
            async () => undefined,
            () => undefined,
        );

        await expect(controller.keepAlive()).resolves.toBe(false);
        await controller.open(firstLocation);
        await expect(controller.keepAlive()).resolves.toBe(true);
        expect(keepImageAlive).toHaveBeenCalledOnce();
        expect(keepImageAlive).toHaveBeenCalledWith(7);
    });

    it('closes a stale late open without replacing the current session', async () => {
        let resolveFirst: ((value: OpenedImage) => void) | undefined;
        const openImage = vi
            .fn()
            .mockImplementationOnce(
                async () =>
                    await new Promise<OpenedImage>((resolve) => {
                        resolveFirst = resolve;
                    }),
            )
            .mockResolvedValueOnce(opened(2));
        const closeImage = vi.fn(async () => undefined);
        const controller = new ImageSessionController(
            { openImage, keepImageAlive: vi.fn(), refreshImage: vi.fn(), closeImage },
            async () => undefined,
            () => undefined,
        );

        const first = controller.open(firstLocation);
        await expect(controller.open(secondLocation)).resolves.toMatchObject({ sessionId: 2 });
        resolveFirst?.(opened(1));
        await expect(first).resolves.toBeNull();
        expect(closeImage).toHaveBeenCalledWith(1);
        expect(controller.snapshot().sessionId).toBe(2);
    });

    it('invalidates and closes the active session on disposal', async () => {
        const closeImage = vi.fn(async () => undefined);
        const invalidate = vi.fn(async () => undefined);
        const controller = new ImageSessionController(
            {
                openImage: vi.fn(async () => opened(9)),
                keepImageAlive: vi.fn(),
                refreshImage: vi.fn(),
                closeImage,
            },
            invalidate,
            () => undefined,
        );
        await controller.open(firstLocation);

        await controller.dispose();
        expect(invalidate).toHaveBeenCalledWith(9);
        expect(closeImage).toHaveBeenCalledWith(9);
        expect(controller.snapshot().sessionId).toBeNull();
    });

    it('cancels an active open without discarding the existing session', async () => {
        const openImage = vi
            .fn()
            .mockResolvedValueOnce(opened(3))
            .mockImplementationOnce(
                async (_location: ImageLocation, options?: { signal?: AbortSignal }) =>
                    await new Promise<OpenedImage>((_resolve, reject) => {
                        options?.signal?.addEventListener(
                            'abort',
                            () => reject(new DOMException('cancelled', 'AbortError')),
                            { once: true },
                        );
                    }),
            );
        const controller = new ImageSessionController(
            {
                openImage,
                keepImageAlive: vi.fn(),
                refreshImage: vi.fn(),
                closeImage: vi.fn(),
            },
            async () => undefined,
            () => undefined,
        );
        await controller.open(firstLocation);

        const replacement = controller.open(secondLocation);
        controller.cancelOpen();

        await expect(replacement).rejects.toMatchObject({ name: 'AbortError' });
        expect(controller.snapshot()).toMatchObject({ sessionId: 3, location: firstLocation, opening: false });
    });
});
