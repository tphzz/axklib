import type { ImageTransport } from '../../lib/transport';
import type { AuditionWorkflow } from '../audition/workflow.svelte';
import type { PreparedVoice } from '../../lib/audio/heldVoices';
import { planDirectPlayback } from '../../lib/audio/directPlaybackSchedule';
import { sampleSnapshot, type ProgramEditorTarget } from '../../lib/objectEditing';
import type { EditorValues } from '../object-editor/draft.svelte';
import type { ObjectEditorDocument, ObjectEditorWorkflow } from '../object-editor/workflow.svelte';
import { ProgramDraft } from '../devices/a-series/program/draft.svelte';
import { BankDraft } from '../devices/a-series/bank/draft.svelte';
import { mappingMembers } from '../devices/a-series/bank/mappingMembers';
import { previewSamples, programCoverage } from '../devices/a-series/program/visualization';
import { sampleMappingRange } from '../devices/a-series/sample/mapping';
import { draftPlaybackPlan } from '../devices/a-series/sample/audition';
import { parameterDomain } from '../devices/a-series/sample/formatCapabilities';
import { matchesMapping, mappingVoiceValues } from './playbackPlan';
import { MappingPcmCache } from './pcmCache';
import type { MappingRole } from './protocol';

export interface MappingAudio {
    play(
        document: ObjectEditorDocument,
        role: MappingRole,
        token: string,
        note: number,
        velocity: number,
    ): Promise<void>;
    release(token: string): void;
}
interface ResolvedVoice {
    document: ObjectEditorDocument;
    values: EditorValues;
    gain: number;
}
export class MappingAudition implements MappingAudio {
    private readonly pcm: MappingPcmCache;
    constructor(
        private readonly editors: ObjectEditorWorkflow,
        transport: ImageTransport,
        private readonly audio: Pick<AuditionWorkflow, 'playVoices' | 'releaseVoices' | 'state'>,
    ) {
        this.pcm = new MappingPcmCache(transport);
    }
    release(token: string): void {
        this.audio.releaseVoices(token);
    }
    invalidate(): void {
        this.pcm.reset();
    }
    play(
        document: ObjectEditorDocument,
        role: MappingRole,
        token: string,
        note: number,
        velocity: number,
    ): Promise<void> {
        return this.audio
            .playVoices(document.sessionId, document.detail!.object.id, token, async (context, signal) => {
                const revision = document.detail!.image.revision,
                    draftRevision = document.draft.revision;
                const voices = await this.resolve(document, role, note, velocity, signal);
                const revisions = this.editors.documents
                    .filter((item) => item.sessionId === document.sessionId)
                    .map((item) => ({ item, detail: item.detail, revision: item.draft.revision, phase: item.phase }));
                const unique = [
                    ...new Map(voices.map((voice) => [voice.document.detail!.object.id, voice.document])).values(),
                ];
                const buffers = new Map<string, AudioBuffer>();
                const sourceKeys = new Map(
                    unique.map((sample) => {
                        const id = sample.detail!.object.id;
                        const sources = sampleSnapshot(sample.detail)?.sources ?? [];
                        return [
                            id,
                            sources.length
                                ? JSON.stringify(
                                      sources.map((source) => [
                                          source.objectId,
                                          source.role,
                                          source.frames,
                                          source.sampleRate,
                                      ]),
                                  )
                                : id,
                        ];
                    }),
                );
                this.pcm.retain(document.sessionId, revision, new Set(sourceKeys.values()));
                await boundedMap(unique, async (sample) => {
                    const id = sample.detail!.object.id;
                    buffers.set(
                        id,
                        await this.pcm.load(document.sessionId, revision, id, context, signal, sourceKeys.get(id)!),
                    );
                });
                signal.throwIfAborted();
                if (document.detail!.image.revision !== revision || document.draft.revision !== draftRevision)
                    throw new Error('The mapping changed while audio was preparing. Press the key again.');
                if (
                    revisions.some(
                        ({ item, detail, revision, phase }) =>
                            item.detail !== detail ||
                            item.draft.revision !== revision ||
                            item.phase !== phase ||
                            item.conflict,
                    )
                )
                    throw new Error(
                        'A matching Sample or Bank changed while audio was preparing. Press the key again.',
                    );
                return voices.map((voice) =>
                    this.prepare(voice, note, context, buffers.get(voice.document.detail!.object.id)!),
                );
            })
            .then(() => {
                if (
                    this.audio.state.mapping &&
                    this.audio.state.objectId === document.detail?.object.id &&
                    this.audio.state.status === 'failed'
                )
                    throw new Error(this.audio.state.error);
            });
    }
    private async load(owner: ObjectEditorDocument, id: string, signal: AbortSignal): Promise<ObjectEditorDocument> {
        signal.throwIfAborted();
        const sample = await this.editors.load(owner.sessionId, id);
        signal.throwIfAborted();
        if (
            !sample?.detail ||
            sample.detail.image.revision !== owner.detail!.image.revision ||
            sample.phase !== 'editable' ||
            sample.conflict ||
            (sample.draft.dirty ? sample.validation : Object.values(sample.inputErrors).find(Boolean))
        )
            throw new Error('A matching Sample or Bank is unavailable or stale; no partial group was played.');
        return sample;
    }
    private async bank(
        owner: ObjectEditorDocument,
        bank: ObjectEditorDocument,
        signal: AbortSignal,
    ): Promise<ObjectEditorDocument[]> {
        const members = mappingMembers(bank.detail!);
        if (
            (sampleSnapshot(bank.detail)?.bankOverrides?.members ?? []).some(
                (member) => !members.some((known) => member.objectId === known.id),
            )
        )
            throw new Error('This Bank has unresolved members; no partial group was played.');
        return boundedMap(members, (member) => this.load(owner, member.id, signal));
    }
    private async resolve(
        document: ObjectEditorDocument,
        role: MappingRole,
        note: number,
        velocity: number,
        signal: AbortSignal,
    ): Promise<ResolvedVoice[]> {
        if (role === 'program') {
            const snapshot = document.detail!.editing;
            if (snapshot?.profile !== 'a-series/program' || !(document.draft instanceof ProgramDraft))
                throw new Error('Program audition is unavailable.');
            const assignments = document.draft.assignments.filter((row) => row.name);
            const ids = new Set<string>();
            for (const row of assignments) {
                const target = snapshot.targets.find((target) => target.objectId === row.targetObjectId);
                if (!target?.available || row.kind === 'UNKNOWN')
                    throw new Error('This Program has unresolved assignments; no partial group was played.');
                ids.add(target.objectId);
                target.members.forEach((member) => {
                    if (!member.objectId) throw new Error('Unresolved Program member');
                    ids.add(member.objectId);
                });
            }
            await boundedMap([...ids], (id) => this.load(document, id, signal));
            for (const row of assignments)
                if (row.kind === 'SBAC')
                    await this.bank(document, this.editors.find(document.sessionId, row.targetObjectId!)!, signal);
            const lookup = (id: string) => this.editors.find(document.sessionId, id);
            return assignments.flatMap((row) =>
                previewSamples(snapshot, row, lookup).flatMap((sample) => {
                    const range = programCoverage(sample, document.draft.values, row.id);
                    return range && matchesMapping(range, note, velocity)
                        ? [
                              {
                                  document: lookup(sample.objectId)!,
                                  values: mappingVoiceValues(
                                      sample.values,
                                      document.draft.values,
                                      row.id,
                                      parameterDomain(
                                          sampleSnapshot(lookup(sample.objectId)!.detail)!,
                                          'coarse_tune',
                                      ) ?? undefined,
                                  ),
                                  gain: Number(document.draft.values.level ?? 127) / 127,
                              },
                          ]
                        : [];
                }),
            );
        }
        const samples = role === 'sample' ? [document] : await this.bank(document, document, signal);
        return samples.flatMap((sample) => {
            const values = { ...sample.draft.storedValues };
            if (document.draft instanceof BankDraft)
                for (const unit of document.draft.units)
                    for (const key of unit.keys)
                        if (document.draft.isOverridden(key)) values[key] = document.draft.storedValues[key]!;
            const range = sampleMappingRange(values);
            if (!range) throw new Error('A Sample mapping is invalid; no partial group was played.');
            return matchesMapping(range, note, velocity) ? [{ document: sample, values, gain: 1 }] : [];
        });
    }
    private prepare(voice: ResolvedVoice, note: number, context: AudioContext, buffer: AudioBuffer): PreparedVoice {
        const { values } = voice,
            snapshot = sampleSnapshot(voice.document.detail);
        if (!snapshot || snapshot.profile !== 'a-series/sample')
            throw new Error('Matching Wave Data cannot be previewed.');
        const repeating = [1, 2].includes(Number(values.loop_mode));
        if (
            [
                'root_key',
                'fine_tune_cents',
                'playback.start_frame',
                'playback.length_frames',
                ...(repeating ? ['loop_start_frame', 'loop_length_frames'] : []),
            ].some((key) => snapshot.blockedParameters.includes(key))
        )
            throw new Error('Preview cannot combine different stereo channel playback settings.');
        const plan = draftPlaybackPlan(values, buffer.sampleRate, buffer.sampleRate, note);
        if (
            !Number.isSafeInteger(plan.start) ||
            !Number.isSafeInteger(plan.length) ||
            plan.start < 0 ||
            plan.length < 1 ||
            plan.start + plan.length > buffer.length ||
            !Number.isFinite(plan.speed) ||
            plan.speed <= 0 ||
            !Number.isInteger(plan.loopMode) ||
            plan.loopMode < 0 ||
            plan.loopMode > 5
        )
            throw new Error('Draft playback exceeds the stored Wave Data.');
        const loopStart = Number(values.loop_start_frame),
            loopLength = Number(values.loop_length_frames);
        if (
            repeating &&
            (!Number.isSafeInteger(loopStart) ||
                !Number.isSafeInteger(loopLength) ||
                loopStart < plan.start ||
                loopLength <= 0 ||
                loopStart + loopLength > plan.start + plan.length)
        )
            throw new Error('Repeating playback requires a nonempty loop inside the playback window.');
        const rate = buffer.sampleRate;
        const schedule = planDirectPlayback(
            {
                loopMode: plan.loopMode,
                loopStartFrame: loopStart - plan.start,
                loopLengthFrames: loopLength,
                frameCount: plan.length,
                sampleRate: rate,
            },
            plan.length / rate,
            'interactive',
        );
        return {
            buffer: plan.reverse ? this.pcm.reverse(buffer, plan.start, plan.length, context) : buffer,
            start: plan.reverse ? 0 : plan.start / rate,
            length: plan.length / rate,
            speed: plan.speed,
            gain: plan.gain * voice.gain,
            pan: plan.pan,
            loop: schedule.loop,
            loopStart: plan.start / rate + schedule.loopStartSeconds,
            loopEnd: plan.start / rate + schedule.loopEndSeconds,
            stopAfter: schedule.stopAfterSeconds,
        };
    }
}
export async function boundedMap<T, R>(items: T[], run: (item: T) => Promise<R>): Promise<R[]> {
    const results: R[] = new Array(items.length);
    let next = 0;
    let failed = false;
    await Promise.all(
        Array.from({ length: Math.min(4, items.length) }, async () => {
            for (;;) {
                if (failed) return;
                const index = next++;
                if (index >= items.length) return;
                try {
                    results[index] = await run(items[index]!);
                } catch (error) {
                    failed = true;
                    throw error;
                }
            }
        }),
    );
    return results;
}
