import type { ClientUploadSource } from '../clientUploadSource';
import type { ClientUploadLocation, FileLocation, InputFileLocation } from '../storageLocations';
import type { AudioSourceInfo } from '../transport';

export interface AudioImportRow {
    id: number;
    candidate: ClientUploadSource | FileLocation;
    fileName: string;
    source?: InputFileLocation;
    upload?: ClientUploadLocation;
    inspection?: AudioSourceInfo;
    targetSampleRate?: number;
    inspectionRevision: number;
    sampleName: string;
    waveformNames: string[];
    rootKey: number;
    progress: number;
    status: 'waiting' | 'uploading' | 'checking' | 'ready' | 'failed' | 'removing';
    error: string;
}
