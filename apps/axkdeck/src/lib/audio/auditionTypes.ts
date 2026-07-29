import type { DiagnosticLevel } from '../diagnostics';

export interface AuditionState {
    objectId: string | null;
    status: 'idle' | 'preparing' | 'playing' | 'failed';
    playheadFrame: number;
    error?: string;
    errorCode?: string;
    errorContext?: unknown;
}

export interface AuditionDiagnosticEvent extends Record<string, unknown> {
    event: string;
    level: DiagnosticLevel;
    playbackId: string;
    objectId: string;
    elapsedMs: number;
}

export type AuditionSequenceResult =
    | { status: 'completed'; playedCount: number; skippedCount: 0 }
    | {
          status: 'failed';
          playedCount: 0;
          skippedCount: number;
          error: string;
          errorCode?: string;
          errorContext?: unknown;
          failedObjectId: string;
      }
    | { status: 'cancelled'; playedCount: 0; skippedCount: number };

export interface AuditionControllerOptions {
    cacheBudgetBytes?: number;
    maximumCacheEntries?: number;
    maximumWorkingSetBytes?: number;
    maximumSequenceWorkingSetBytes?: number;
}

export interface PlaybackRun {
    id: string;
    objectId: string;
    startedAt: number;
    diagnosticsEnabled: boolean;
    oneShot: boolean;
}

export interface PlaybackDescriptor {
    objectId: string;
    sampleRate: number;
    channels: number;
    sampleWidthBytes: number;
    frameCount: number;
    wavSizeBytes: number;
    loopMode: number;
    loopModeLabel: string;
    loopStartFrame: number;
    loopLengthFrames: number;
    warnings: string[];
}

export interface CachedAudition {
    key: string;
    sessionId: number;
    objectId: string;
    descriptor: PlaybackDescriptor;
    buffer: AudioBuffer;
    weightBytes: number;
    transient: boolean;
}

export type AuditionDiagnosticSink = (event: AuditionDiagnosticEvent) => void;
export type AuditionAssetEventSink = (run: PlaybackRun, event: string, fields?: Record<string, unknown>) => void;
