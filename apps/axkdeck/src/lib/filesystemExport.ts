import type { components } from './generated/axklibApiV1';
import type { JobState } from './transport';

export type FilesystemExportDestination = components['schemas']['ImageFilesystemExportDestination'];
export type FilesystemExportInspection = components['schemas']['ImageFilesystemExportInspection'];
export type FilesystemExportResult = components['schemas']['ImageFilesystemExportResult'];
export type FilesystemExportLayout = components['schemas']['ImageFilesystemExportLayout'];

export interface FilesystemExportDriver {
    inspect(revision: number, entryIds: string[], layout?: FilesystemExportLayout): Promise<FilesystemExportInspection>;
    execute(
        revision: number,
        entryIds: string[],
        destination: FilesystemExportDestination,
        update: (job: JobState) => void,
        layout?: FilesystemExportLayout,
    ): Promise<JobState>;
    observe(jobId: number, update: (job: JobState) => void): Promise<JobState>;
    cancel(jobId: number): Promise<void>;
}

export type FilesystemExportRoute = 'workspace' | 'computer';

export interface FilesystemExportTarget {
    destination: FilesystemExportDestination;
    publish(result: FilesystemExportResult): Promise<void>;
    cancelPublication?(): Promise<void>;
}

export interface FilesystemExportActions extends FilesystemExportDriver {
    drag?: {
        reserve(sizeBytes: number): Promise<string>;
        prepare(ticket: string, contentPath: string): Promise<void>;
        start(ticket: string): Promise<void>;
        cancel(ticket: string): Promise<void>;
    };
    directComputer: boolean;
    desktop: boolean;
    chooseDestination(route: FilesystemExportRoute, suggestedName: string): Promise<FilesystemExportTarget | null>;
    release(result: FilesystemExportResult): Promise<void>;
}
