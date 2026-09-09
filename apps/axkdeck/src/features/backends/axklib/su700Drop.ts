import type { ClientFilesystemImportEntry } from '../../../lib/filesystemImport';

// Admission only: the server validates the FAT tree and all SU700 references.
export async function isFloppyCandidate(entries: ClientFilesystemImportEntry[]): Promise<boolean> {
    if (entries.length !== 1 || entries[0].directory || entries[0].relativePath.length !== 1) return false;
    const source = entries[0].source;
    if (source.size < 512 || source.size > 4 * 1024 * 1024) return false;
    const bytes = await (await source.readChunk(0, 512)).arrayBuffer();
    if (bytes.byteLength < 512) return false;
    const view = new DataView(bytes);
    return view.getUint16(11, true) === 512 && [1, 2].includes(view.getUint8(16)) && view.getUint16(22, true) > 0;
}
