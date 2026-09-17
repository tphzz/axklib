import type { FilesystemImportEntry, FilesystemEditSource } from './filesystem';
import type { InputFileLocation } from './storageLocations';
import type { JobState } from './transport';
import type { ClientUploadSource } from './clientUploadSource';
import type { FilesystemEntry } from './filesystem';

export type FilesystemImportSourceEntry =
    | { relativePath: string[]; directory: true }
    | { relativePath: string[]; directory: false; source: InputFileLocation };

export type ClientFilesystemImportEntry =
    | { relativePath: string[]; directory: true }
    | { relativePath: string[]; directory: false; source: ClientUploadSource };

export type FilesystemDropReader = (
    signal: AbortSignal,
    progress: (message: string) => void,
) => Promise<ClientFilesystemImportEntry[]>;

export interface FilesystemImageImporter {
    label: string;
    enabled: boolean;
    busy: boolean;
    open(entries?: ClientFilesystemImportEntry[], target?: FilesystemEntry): Promise<boolean>;
}

export interface FilesystemImportDriver {
    inspectInputs(inputs: InputFileLocation[], update: (job: JobState) => void): Promise<JobState>;
    inspectDestination(
        revision: number,
        parentEntryId: string,
        entries: FilesystemImportEntry[],
        update: (job: JobState) => void,
    ): Promise<JobState>;
    observe(jobId: number, update: (job: JobState) => void): Promise<JobState>;
    cancel(jobId: number): Promise<void>;
}

export interface FilesystemImportActions extends FilesystemImportDriver {
    images?: {
        inspect(source: InputFileLocation, update: (job: JobState) => void): Promise<JobState>;
        release(inspectionToken: string): Promise<void>;
    };
    supportsClientUploads: boolean;
    chooseFiles(title?: string): Promise<InputFileLocation[] | null>;
    chooseDirectory(
        signal: AbortSignal,
        progress: (message: string) => void,
    ): Promise<FilesystemImportSourceEntry[] | null>;
    upload(
        files: ClientUploadSource[],
        signal: AbortSignal,
        progress: (message: string) => void,
    ): Promise<InputFileLocation[]>;
    release(inputs: FilesystemEditSource[]): Promise<void>;
}
