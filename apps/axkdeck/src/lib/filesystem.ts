import type { InputFileLocation } from './storageLocations';
import type { JobState } from './transport';
import type {
    FilesystemExportDestination,
    FilesystemExportInspection,
    FilesystemExportLayout,
} from './filesystemExport';
import type { components } from './generated/axklibApiV1';

export type FilesystemInputSnapshot = components['schemas']['FilesystemInputSnapshot'];
export type FilesystemInputInspection = components['schemas']['FilesystemInputInspectionResult'];
export type FilesystemImportEntry = components['schemas']['FilesystemImportEntry'];
export type FilesystemImportInspection = components['schemas']['ImageFilesystemImportInspectionResult'];
export type FilesystemEditResult = components['schemas']['ImageFilesystemEditResult'];

export interface FilesystemAttribute {
    code: string;
    label: string;
    value: string;
    description: string;
    summary: string;
}

export interface FilesystemEntry {
    id: string;
    parentId: string | null;
    rootId: string;
    ancestorIds: string[];
    name: string;
    path: string;
    kind: 'root' | 'partition' | 'directory' | 'file';
    sizeBytes: number | null;
    childCount: number;
    objectId: string | null;
    contentScopeId: string | null;
    interpretation: string;
    storage: string;
    issue: string;
    filesystemMetadata: boolean;
    rawAttributes: string;
    attributes: FilesystemAttribute[];
}

export interface FilesystemQuery {
    parentId?: string;
    rootId?: string;
    query?: string;
    entryId?: string;
    objectId?: string;
    contentScopeId?: string;
    offset?: number;
    limit?: number;
}

export interface FilesystemPage {
    revision: number;
    available: boolean;
    filesystemName: string;
    deviceView: string | null;
    items: FilesystemEntry[];
    totalCount: number;
    rootCapabilities: FilesystemRootCapabilities[];
}

export interface FilesystemRootCapabilities {
    rootId: string;
    createDirectory: boolean;
    putFile: boolean;
    deleteEntry: boolean;
    renameEntry: boolean;
    moveEntry: boolean;
    maximumNameBytes: number;
    namePolicy: 'PRESERVE' | 'FAT_8_3_UPPERCASE';
    namePattern: string;
    nameHint: string;
    supportedImports: string[];
}

export type FilesystemEditSource =
    | InputFileLocation
    | {
          kind: 'image-entry';
          displayName: string;
          reference: { inspectionToken: string; entryId: string };
      };

export type FilesystemImageInspection = components['schemas']['FilesystemImageInspection'];

export type FilesystemEdit =
    | { kind: 'CREATE_DIRECTORY'; parentEntryId: string; relativePath: string[] }
    | {
          kind: 'PUT_FILE';
          parentEntryId: string;
          relativePath: string[];
          source: FilesystemEditSource;
          expectedSource: FilesystemInputSnapshot;
          conflict: 'SKIP' | 'REPLACE';
      }
    | { kind: 'DELETE'; entryId: string; recursive: boolean }
    | { kind: 'RENAME'; entryId: string; newName: string }
    | { kind: 'MOVE'; entryId: string; destinationParentEntryId: string };

export interface FilesystemTransport {
    startFilesystemImportInspection(
        sessionId: number,
        expectedRevision: number,
        parentEntryId: string,
        entries: FilesystemImportEntry[],
    ): Promise<JobState>;
    startFilesystemInputInspection(inputs: InputFileLocation[]): Promise<JobState>;
    startFilesystemImageInspection(source: InputFileLocation): Promise<JobState>;
    releaseFilesystemImageInspection(inspectionToken: string): Promise<void>;
    filesystem(sessionId: number, query?: FilesystemQuery): Promise<FilesystemPage>;
    startFilesystemEdits(sessionId: number, expectedRevision: number, edits: FilesystemEdit[]): Promise<JobState>;
    inspectFilesystemExport(
        sessionId: number,
        expectedRevision: number,
        entryIds: string[],
        layout?: FilesystemExportLayout,
    ): Promise<FilesystemExportInspection>;
    startFilesystemExport(
        sessionId: number,
        expectedRevision: number,
        entryIds: string[],
        destination: FilesystemExportDestination,
        layout?: FilesystemExportLayout,
    ): Promise<JobState>;
}

// A bound image session, independent of HTTP, native processes and device object types.
export interface FilesystemAccess {
    inspect(query?: FilesystemQuery): Promise<FilesystemPage>;
}

export interface FilesystemMutationDriver {
    execute(revision: number, edits: FilesystemEdit[], update: (job: JobState) => void): Promise<JobState>;
    observe(jobId: number, update: (job: JobState) => void): Promise<JobState>;
    cancel(jobId: number): Promise<void>;
    refresh(): Promise<void>;
}

// Drivers use this only when submission was definitively rejected before any write.
export class FilesystemWriteRejected extends Error {}
