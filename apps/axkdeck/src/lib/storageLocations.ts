export interface FileRef {
    rootId: string;
    relativePath: string;
}

export type DirectoryRef = FileRef;

export interface ServerFileLocation {
    kind: 'server-file';
    reference: FileRef;
    displayName: string;
}

export interface ServerDirectoryLocation {
    kind: 'server-directory';
    reference: DirectoryRef;
    displayName: string;
}

export interface UploadRef {
    uploadId: string;
}

export type UploadKind = 'AUDIO' | 'PACKAGE' | 'MANIFEST';

export interface ClientUploadLocation {
    kind: 'client-upload';
    reference: UploadRef;
    uploadKind: UploadKind;
    displayName: string;
}

export type FileLocation = ServerFileLocation;
export type DirectoryLocation = ServerDirectoryLocation;
export type InputFileLocation = FileLocation | ClientUploadLocation;

export interface SandboxRoot {
    id: string;
    displayName: string;
    writable: boolean;
}

export interface SandboxEntry {
    name: string;
    relativePath: string;
    kind: 'FILE' | 'DIRECTORY';
    size: number | null;
}

export interface DirectoryListing {
    directory: DirectoryRef;
    entries: SandboxEntry[];
    truncated: boolean;
    nextCursor: string | null;
}

export function serverFileLocation(reference: FileRef, displayName?: string): ServerFileLocation {
    return {
        kind: 'server-file',
        reference,
        displayName: displayName ?? (reference.relativePath || reference.rootId),
    };
}

export function serverDirectoryLocation(reference: DirectoryRef, displayName?: string): ServerDirectoryLocation {
    return {
        kind: 'server-directory',
        reference,
        displayName: displayName ?? (reference.relativePath || reference.rootId),
    };
}

export function clientUploadLocation(
    reference: UploadRef,
    uploadKind: UploadKind,
    displayName: string,
): ClientUploadLocation {
    return { kind: 'client-upload', reference, uploadKind, displayName };
}

export function locationKey(location: FileLocation | DirectoryLocation): string {
    return JSON.stringify([location.reference.rootId, location.reference.relativePath]);
}

export function inputLocationKey(location: InputFileLocation): string {
    return location.kind === 'client-upload'
        ? JSON.stringify(['upload', location.reference.uploadId])
        : locationKey(location);
}
