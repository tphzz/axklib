import { AxklibApiError } from './httpErrors';
import type { AxklibHttpApiClient, DownloadArchiveSnapshot } from './httpApiClient';
import type { ClientDownload } from './transport';
import type { FileRef } from './storageLocations';

const maximumRendererDownloadBytes = 256 * 1024 * 1024;

function requireRendererCapacity(size: number, kind: string): void {
    if (!Number.isSafeInteger(size) || size < 0 || size > maximumRendererDownloadBytes) {
        throw new AxklibApiError(
            'download_too_large',
            `This ${kind} is too large to hold safely in the browser. Use the desktop save workflow.`,
            413,
        );
    }
}

export async function downloadServerFile(
    client: AxklibHttpApiClient,
    reference: FileRef,
    displayName: string,
): Promise<ClientDownload> {
    const metadata = await client.inspectDownload(reference);
    const limits = await client.serverLimits();
    const rangeLimit = limits.maximumDownloadRangeBytes;
    if (!Number.isSafeInteger(rangeLimit) || rangeLimit <= 0) {
        throw new Error('axklib-server advertised an invalid download range limit');
    }
    requireRendererCapacity(metadata.size, 'download');
    const parts: Blob[] = [];
    for (let start = 0; start < metadata.size; start += rangeLimit) {
        const end = Math.min(metadata.size, start + rangeLimit) - 1;
        const response = await client.openDownload(reference, { start, end }, metadata.revision);
        parts.push(await client.readBoundedBlob(response, end - start + 1, rangeLimit));
    }
    const filename = reference.relativePath.split('/').pop() || displayName;
    return { filename, blob: new Blob(parts, { type: 'application/octet-stream' }) };
}

export async function readDirectoryArchive(
    client: AxklibHttpApiClient,
    archive: DownloadArchiveSnapshot,
): Promise<ClientDownload> {
    requireRendererCapacity(archive.sizeBytes, 'archive');
    const response = await client.openDirectoryArchive(archive);
    return {
        filename: archive.filename,
        blob: await client.readBoundedBlob(response, archive.sizeBytes, maximumRendererDownloadBytes),
    };
}
