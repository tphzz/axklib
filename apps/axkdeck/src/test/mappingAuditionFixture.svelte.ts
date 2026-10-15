import { AuditionController, type AuditionState } from '../lib/audio/auditionController';
import type { ImageTransport } from '../lib/transport';
import { ObjectEditorWorkflow } from '../features/object-editor/workflow.svelte';
import { MappingAudition } from '../features/program-mapping/audition';
import { mappingDetail } from './mappingEditorFixture';
import { fixtureFrames, fixtureRate, fixtureWave } from './sampleEditorAudio';

export class MappingAuditionFixture {
    state = $state<AuditionState>({ objectId: null, status: 'idle', playheadFrame: 0 });
    voices = $state(0);
    notes = $state<{ note: number; velocity: number }[]>([]);
    outputState = $state('uncreated');
    peak = $state(0);
    private readonly controller: AuditionController;
    readonly mapping: MappingAudition;
    constructor() {
        const lane = {
            role: 'MONO' as const,
            sourceObjectId: 'wave',
            sampleWidthBytes: 2,
            loopStartFrame: 0,
            loopLengthFrames: 0,
            sampleRate: fixtureRate,
            frameCount: fixtureFrames,
            contentOffsetBytes: 0,
            wavSizeBytes: 44 + fixtureFrames * 2,
        };
        const transport = {
            audioIdentity: () => ({ imageId: 'image-1', documentId: 'document-1', revision: 1 }),
            objectDetail: async (_: number, id: string) => {
                const detail = mappingDetail(id);
                if (detail.editing?.profile === 'a-series/sample') {
                    detail.editing.playbackWindow = { start_frame: 0, length_frames: fixtureFrames };
                    detail.editing.maximumFrames = fixtureFrames;
                    detail.editing.parameters = {
                        ...detail.editing.parameters,
                        loop_mode: 1,
                        loop_start_frame: 0,
                        loop_length_frames: fixtureFrames,
                    };
                }
                return detail;
            },
            prepareAuditionBundle: async (_session: number, objectIds: readonly string[]) => ({
                auditionId: 'mapping-fixture',
                imageId: 'image-1',
                documentId: 'document-1',
                revision: 1,
                contentSizeBytes: lane.wavSizeBytes,
                clips: [
                    {
                        objectId: objectIds[0]!,
                        direction: 'FORWARD' as const,
                        looping: false,
                        warnings: [],
                        lanes: [lane],
                    },
                ],
            }),
            readAuditionContent: async () => fixtureWave(),
            deleteAudition: async () => {},
        } as unknown as ImageTransport;
        const editors = new ObjectEditorWorkflow({
            transport,
            refresh: async () => {},
            stopPlayback: () => {},
            status: () => {},
        });
        this.controller = new AuditionController(
            transport,
            (state) => {
                this.state = state;
            },
            () => {},
            () => false,
        );
        const fixture = this;
        this.mapping = new MappingAudition(editors, transport, {
            get state() {
                return fixture.state;
            },
            releaseVoices: (token) => this.controller.releaseVoices(token),
            playVoices: (session, id, token, prepare) =>
                this.controller.playVoices(session, id, token, async (context, signal) => {
                    const voices = await prepare(context, signal);
                    this.voices = voices.length;
                    this.outputState = context.state;
                    this.peak = voices.reduce(
                        (peak, voice) => Math.max(peak, ...voice.buffer.getChannelData(0).subarray(0, 100)),
                        0,
                    );
                    return voices;
                }),
        });
    }
    async dispose(): Promise<void> {
        this.mapping.invalidate();
        await this.controller.dispose();
    }
}
