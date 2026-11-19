import type { ConversionInput, ConversionPlan, ConversionProfile } from '../lib/conversions';
import type { JobState, RetainedDownload } from '../lib/transport';
import type { FileRef } from '../lib/storageLocations';

export const waveSource = { kind: 'DOCUMENT' as const, sessionId: 7, documentId: 'a-series', expectedRevision: 4 };
export const waveProfile: ConversionProfile = {
    id: 'fixture-wave-profile',
    sourceFormat: 'yamaha.a-series',
    destinationFormat: 'yamaha.tx16w',
    outputKind: 'NEW_IMAGE',
    name: 'Yamaha Wave disk',
    optionsSchema: {},
    route: {
        medium: 'fat12-720k',
        targetLabel: 'TX16W · Yamaha Wave disk',
        intents: ['WAVE_DATA'],
        editorCapabilities: ['ordered-wave-selection', 'new-image-file'],
        priority: 0,
    },
};
export const waveRows = ['Piano Low', 'Piano Mid', 'Piano High'].map((name, i) => ({
    id: 'wave-' + i,
    name,
    type: 'SMPL',
    volumeName: 'Studio',
}));
export const waveDownload: RetainedDownload = {
    archiveId: 'fixture',
    filename: 'conversion.img',
    sizeBytes: 737280,
    expiresInSeconds: 300,
    contentPath: '/api/v1/download-archives/fixture/content',
};
export function waveFixturePlan(input: ConversionInput): ConversionPlan {
    const ids = input.options.waveIds as string[];
    return {
        planId: 'wave-plan',
        expiresAt: Date.now() + 60000,
        jobId: null,
        state: 'READY',
        report: {
            profileId: input.profileId,
            fidelity: input.fidelity,
            executable: input.fidelity !== 'STRICT',
            offset: 0,
            capacity: null,
            actionCount: ids.length,
            issueCount: 1,
            destination: { kind: 'NEW_IMAGE', sizeBytes: 737280, sha256: null },
            actions: ids.map((id, i) => ({
                name:
                    (waveRows.find((row) => row.id === id)?.name ?? 'WAVE')
                        .toUpperCase()
                        .replaceAll(' ', '_')
                        .slice(0, 8) +
                    '.W' +
                    String(i + 1).padStart(2, '0'),
                sourceObject: id,
                kind: 'wave',
                action: 'create',
                details: [],
            })),
            issues: [
                {
                    id: 'precision',
                    impact: 'LOSS',
                    code: 'tx16w.precision',
                    message: 'Audio is rounded to the nearest 12-bit level.',
                    sourceObject: ids[0],
                    sourceParameter: 'Wave Data',
                    targetObject: 'Wave',
                    targetParameter: 'Precision',
                },
            ],
        },
    };
}
export function waveFixtureTransport() {
    let file: FileRef | undefined;
    return {
        conversionProfiles: async () => [waveProfile],
        inspectConversion: async () => ({
            sourceFormat: waveProfile.sourceFormat,
            documentId: waveSource.documentId,
            offset: 0,
            total: waveRows.length,
            profileIds: [waveProfile.id],
            items: waveRows,
        }),
        prepareConversion: async (input: ConversionInput) => {
            file =
                'destination' in input && input.destination.kind === 'NEW_IMAGE' ? input.destination.file : undefined;
            return waveFixturePlan(input);
        },
        executeConversion: async (): Promise<JobState> => ({ jobId: 1, kind: 'conversion', status: 'queued' }),
        conversionExecution: async (): Promise<JobState | null> => ({ jobId: 1, kind: 'conversion', status: 'queued' }),
        waitForJob: async (): Promise<JobState> => ({
            jobId: 1,
            kind: 'conversion',
            status: 'completed',
            result: { planId: 'wave-plan', ...(file ? { file } : { download: waveDownload }) },
        }),
        releaseConversion: async () => {},
        deleteRetainedPackage: async () => {},
    };
}
