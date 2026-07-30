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
        companionDirectories: [],
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
        packageImportAvailable: false,
        packageExportAvailable: false,
        audioExportAvailable: false,
        sequenceExportAvailable: false,
    };
}

describe('ImageSessionController', () => {
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
            { openImage, refreshImage: vi.fn(), closeImage },
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
            { openImage: vi.fn(async () => opened(9)), refreshImage: vi.fn(), closeImage },
            invalidate,
            () => undefined,
        );
        await controller.open(firstLocation);

        await controller.dispose();
        expect(invalidate).toHaveBeenCalledWith(9);
        expect(closeImage).toHaveBeenCalledWith(9);
        expect(controller.snapshot().sessionId).toBeNull();
    });
});
