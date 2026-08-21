import type { ClientUploadSource } from '../../lib/clientUploadSource';
import { nativeFileSource } from '../../lib/nativeFileSource';
import { packageImportExtensionSetCopy, packageImportUploadKind } from '../../lib/packageImportMedia';
import type { ClientUploadLocation, InputFileLocation } from '../../lib/storageLocations';
import type { ImageTransport } from '../../lib/transport';

export interface BatchPackageSource {
    source: InputFileLocation;
    sourceName: string;
    upload: ClientUploadLocation | null;
    localPath: string | null;
}

interface UploadCallbacks {
    progress: (completedFiles: number, currentProgress: number) => void;
}

export async function uploadLocalPackageSources(
    transport: ImageTransport,
    paths: readonly string[],
    signal: AbortSignal,
    callbacks: UploadCallbacks,
): Promise<BatchPackageSource[]> {
    const extensions = packageImportExtensionSetCopy();
    const files = await Promise.all(
        paths.map(async (path) => ({
            file: await nativeFileSource(path, extensions, 'application/octet-stream'),
            localPath: path,
        })),
    );
    return uploadPackageSources(transport, files, signal, callbacks);
}

export async function uploadDroppedPackageSources(
    transport: ImageTransport,
    files: readonly ClientUploadSource[],
    signal: AbortSignal,
    callbacks: UploadCallbacks,
): Promise<BatchPackageSource[]> {
    return uploadPackageSources(
        transport,
        files.map((file) => ({ file, localPath: null })),
        signal,
        callbacks,
    );
}

async function uploadPackageSources(
    transport: ImageTransport,
    files: readonly { file: ClientUploadSource; localPath: string | null }[],
    signal: AbortSignal,
    callbacks: UploadCallbacks,
): Promise<BatchPackageSource[]> {
    const staged: BatchPackageSource[] = [];
    try {
        for (const [index, item] of files.entries()) {
            const uploadKind = packageImportUploadKind(item.file.name);
            if (!uploadKind) throw new Error(`${item.file.name} is not a supported package or A3K archive`);
            const upload = await transport.uploadClientFile(
                item.file,
                uploadKind,
                (sent, total) => callbacks.progress(index, total === 0 ? 0 : sent / total),
                signal,
            );
            staged.push({
                source: upload,
                sourceName: item.file.name,
                upload,
                localPath: item.localPath,
            });
            callbacks.progress(index + 1, 0);
        }
        return staged;
    } catch (error) {
        await Promise.all(staged.map((item) => transport.releaseClientUpload(item.upload!).catch(() => undefined)));
        throw error;
    }
}
