import { describe, expect, it, vi } from 'vitest';
import { nativeWorkspaceFixture } from '../test/nativeWorkspaceFixture';
import type { AxklibHttpApiClient } from './httpApiClient';
import type { HttpImageSessions } from './httpImageSessions';
import type { HttpJobController } from './httpJobController';
import { documentSession, documentTarget } from './httpNativeDocuments';
import { HttpPackageOperations } from './httpPackageOperations';

function fixture() {
    let current = documentSession({ ...nativeWorkspaceFixture(), imageId: 'image-one', revision: 4 });
    const target = Object.freeze({ ...documentTarget(current), scopeId: 'scope-one', expectedRevision: 4 });
    const invoke = vi.fn().mockResolvedValue({ jobId: 'job-one' });
    const operations = new HttpPackageOperations(
        { invoke } as unknown as AxklibHttpApiClient,
        {
            isJob: (value: unknown) => !!value && typeof value === 'object' && 'jobId' in value,
            map: (job: unknown) => job,
        } as unknown as HttpJobController,
        { get: () => current } as unknown as HttpImageSessions,
    );
    return {
        operations,
        invoke,
        target,
        change: (field: 'revision' | 'documentId' | 'remoteId') => {
            current = { ...current, [field]: field === 'revision' ? 5 : 'different-source' };
        },
    };
}

describe('volume package HTTP source binding', () => {
    it('submits exactly the immutable inspected target through the existing wire contract', async () => {
        const f = fixture();
        const destination = { kind: 'DOWNLOAD' as const, directoryName: 'packages' };
        await f.operations.startVolumePackageExport(1, f.target, destination);
        expect(f.invoke).toHaveBeenCalledWith(
            'images.volume_package_export',
            { ...f.target, destination },
            expect.objectContaining({ idempotencyKey: expect.any(String) }),
        );
    });

    it.each(['revision', 'documentId', 'remoteId'] as const)(
        'does not substitute the current %s when submitting an older inspected target',
        async (field) => {
            const f = fixture();
            f.change(field);
            await expect(
                f.operations.startVolumePackageExport(1, f.target, { kind: 'DOWNLOAD', directoryName: 'packages' }),
            ).rejects.toThrow(/changed/i);
            expect(f.invoke).not.toHaveBeenCalled();
        },
    );

    it.each(['revision', 'documentId', 'remoteId'] as const)(
        'rejects a %s change while inspection is pending',
        async (field) => {
            const f = fixture();
            let resolve!: (value: unknown) => void;
            f.invoke.mockReturnValueOnce(new Promise((complete) => (resolve = complete)));
            const inspecting = f.operations.inspectVolumePackageExport(1, 'scope-one');
            f.change(field);
            resolve({ imageId: 'image-one', revision: 4, scopeId: 'scope-one' });
            await expect(inspecting).rejects.toThrow(/changed/i);
        },
    );
});
