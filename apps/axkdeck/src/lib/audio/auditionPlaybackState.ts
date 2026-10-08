import type { CachedAudition, PlaybackDescriptor, AuditionSequenceResult } from './auditionTypes';

export interface ActivePlayback {
    entry: CachedAudition;
    source: AudioBufferSourceNode;
    gain: GainNode;
    startFrame: number;
    startTime: number;
    endTime: number;
    cancelCompletion?: () => void;
    timelineDescriptor: PlaybackDescriptor;
    animationFrame?: number;
}
export interface ScheduledSequenceSegment {
    entry: CachedAudition;
    source: AudioBufferSourceNode;
    startTime: number;
    endTime: number;
    startFrame: number;
    timelineDescriptor: PlaybackDescriptor;
}
export interface ActiveSequence {
    segments: ScheduledSequenceSegment[];
    gain: GainNode;
    completionGeneration: number;
    animationFrame?: number;
    displayedObjectId?: string;
    cancelCompletion?: () => void;
}
export interface SequenceCompletion {
    generation: number;
    memberCount: number;
    oncomplete: (result: AuditionSequenceResult) => void;
}
export interface OutputContextAccess {
    context: AudioContext;
    reused: boolean;
    creationDurationMs: number;
}
