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
    fineTuneCents: number;
    keyLow: number;
    keyHigh: number;
    velocityLow: number;
    velocityHigh: number;
    loopMode: 1 | 4;
    loopStartFrame: number;
    loopLengthFrames: number;
    progress: number;
    status: 'waiting' | 'uploading' | 'checking' | 'inspected' | 'ready' | 'failed' | 'removing';
    error: string;
}
