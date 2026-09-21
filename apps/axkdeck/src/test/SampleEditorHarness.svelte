<script lang="ts">
    import { onDestroy } from 'svelte';
    import { AuditionController, type AuditionState } from '../lib/audio/auditionController';
    import { fixtureFrames, fixtureRate, fixtureWave, fixtureBins } from './sampleEditorAudio';
    import DeviceEditorHost from '../features/object-editor/DeviceEditorHost.svelte';
    import EditorBoundary from '../features/object-editor/EditorBoundary.svelte';
    import WorkspaceShell from '../features/workspace/WorkspaceShell.svelte';
    import SampleEditorCollection from './SampleEditorCollection.svelte';
    import type { ImageSessionWorkflow } from '../features/image-session/workflow.svelte';
    import type { AuditionWorkflow } from '../features/audition/workflow.svelte';
    import { sampleFields } from '../features/devices/a-series/sample/fields';
    import { sampleConversionFixture, sampleFormatFixture } from './sampleFormatFixture';
    import type { ObjectDetail, ImageTransport } from '../lib/transport';
    import type {
        ObjectParameterEdit,
        SampleDuplicationRequest,
        ObjectFormatConversionRequest,
        SampleStorageFormat,
    } from '../lib/objectEditing';
    import type { InspectorSelection } from '../lib/types';
    let selected = $state('Sample A');
    let lowerOpen = $state(false);
    let writes = $state(0);
    let names = $state(['Sample A', 'Sample B', 'Sample C with a very long name that must truncate in narrow panes']);
    const copies = new Map<string, Record<string, unknown>>();
    let closed = $state(false);
    const workspace = new URLSearchParams(window.location.search).has('workspace');
    const stereo = new URLSearchParams(window.location.search).has('short-stereo');
    let rejectConversion = new URLSearchParams(window.location.search).has('conversion-lock');
    let conversionLockPending = false;
    let storedFormats = $state<Record<string, SampleStorageFormat>>({});
    let parameters: Record<string, unknown> = {};
    const unavailableParameters: Record<string, { reason: string; message: string }> = {};
    for (const field of sampleFields) {
        if (field.key.startsWith('playback.')) continue;
        const keys = field.key.split('.');
        let target = parameters;
        for (const key of keys.slice(0, -1)) target = (target[key] ??= {}) as Record<string, unknown>;
        target[keys.at(-1)!] = field.boolean ? false : (field.options?.[0]?.value ?? Math.max(0, field.min));
    }
    Object.assign(parameters, {
        level: 100,
        pan: 0,
        root_key: 60,
        key_low: 0,
        key_high: 127,
        loop_start_frame: 20401,
        loop_length_frames: 42195,
        loop_tempo_hundredths: 12600,
        level_scaling_break1: 0,
        level_scaling_break2: 127,
        level_scaling_level1: 100,
        level_scaling_level2: 127,
        filter_scaling_break1: 24,
        filter_scaling_break2: 96,
        loop_mode: 1,
        velocity_low: 0,
        velocity_high: 127,
    });
    if (stereo) Object.assign(parameters, { portamento_rate: 90, portamento_time: 90 });
    const wavBytes = 44 + fixtureFrames * 2;
    // The server applies nested parameter patches without replacing untouched fields.
    function patchParameters(target: Record<string, unknown>, patch: Record<string, unknown>) {
        for (const [key, value] of Object.entries(patch)) {
            if (value && typeof value === 'object') {
                const group = (target[key] ??= {}) as Record<string, unknown>;
                patchParameters(group, value as Record<string, unknown>);
            } else target[key] = value;
        }
    }
    function detail(name: string): ObjectDetail {
        const format = sampleFormatFixture(storedFormats[name] ?? (stereo ? 'A3000_188' : 'A4000_A5000_224'));
        const currentParameters = structuredClone(
            copies.get(name) ?? { ...parameters, ...(name === 'Sample B' ? { level: 75, root_key: 64 } : {}) },
        );
        for (const [key, capability] of Object.entries(format.parameterCapabilities)) {
            if (capability.available) continue;
            const keys = key.split('.');
            let group = currentParameters;
            for (const part of keys.slice(0, -1)) group = group[part] as Record<string, unknown>;
            delete group[keys.at(-1)!];
        }
        return {
            image: { revision: writes + 1 },
            object: { id: name, key: name, name },
            formatConversion: sampleConversionFixture(format.sampleFormat.format, { volumeName: 'Test' }),
            editing: {
                profile: 'a-series/sample',
                editable: true,
                reason: '',
                payloadSha256: 'a'.repeat(64),
                parameters: currentParameters,
                eqCoefficients: [-15904, 7738, 8192, 15904, -7738],
                blockedParameters: [],
                blockedParameterReasons: {},
                ...format,
                unavailableParameters,
                partitionIndex: 0,
                volumeName: 'Test',
                playbackWindow: { start_frame: 0, length_frames: fixtureFrames },
                canEditPlayback: true,
                maximumFrames: fixtureFrames,
                sources: [],
            },
        } as unknown as ObjectDetail;
    }
    const transport = {
        objectDetail: async (_: number, id: string) => detail(id),
        startObjectParameterEdit: async (_: number, edit: ObjectParameterEdit) => {
            writes++;
            patchParameters(parameters, edit.operation.parameters);
            return { jobId: 1, kind: 'edit', status: 'queued' };
        },
        startSampleDuplication: async (_: number, edit: SampleDuplicationRequest) => {
            writes++;
            const parameters = detail(edit.operation.sample_name).editing!.parameters;
            patchParameters(parameters, edit.operation.parameters);
            copies.set(edit.operation.new_name, parameters);
            names = [...names, edit.operation.new_name];
            return { jobId: 1, kind: 'edit', status: 'queued' };
        },
        startObjectFormatConversion: async (_: number, edit: ObjectFormatConversionRequest) => {
            if (edit.operation.type !== 'convert_sbnk_format') throw new Error('Expected a Sample conversion');
            if (rejectConversion) {
                rejectConversion = false;
                conversionLockPending = true;
                return { jobId: 1, kind: 'edit', status: 'queued' };
            }
            storedFormats[edit.operation.sample_name] =
                edit.operation.target_format === 'a3000_188' ? 'A3000_188' : 'A4000_A5000_224';
            writes++;
            return { jobId: 1, kind: 'edit', status: 'queued' };
        },
        waitForJob: async () => {
            if (conversionLockPending) {
                conversionLockPending = false;
                return {
                    jobId: 1,
                    kind: 'edit',
                    status: 'failed',
                    errorCode: 'entry_in_use',
                    error: 'close open images and wait for active file operations to finish',
                };
            }
            return { jobId: 1, kind: 'edit', status: 'completed' };
        },
        prepareAuditionBundle: async () => ({
            auditionId: 'fixture',
            contentSizeBytes: wavBytes * (stereo ? 2 : 1),
            clips: [
                {
                    objectId: selected,
                    lanes: Array.from({ length: stereo ? 2 : 1 }, (_, index) => ({
                        sampleRate: fixtureRate,
                        frameCount: fixtureFrames,
                        contentOffsetBytes: index * wavBytes,
                        wavSizeBytes: wavBytes,
                    })),
                },
            ],
        }),
        readAuditionContent: async () => {
            const wave = new Uint8Array(fixtureWave());
            const content = new Uint8Array(wave.length * (stereo ? 2 : 1));
            content.set(wave);
            if (stereo) content.set(wave, wave.length);
            return content.buffer;
        },
        deleteAudition: async () => undefined,
    } as unknown as ImageTransport;
    const imageSession = {
        sessionId: 1,
        get revision() {
            return writes + 1;
        },
        confirmEditorLeave: async () => true,
        refresh: async () => undefined,
        currentSourcePreference: () => undefined,
        setStatus: () => undefined,
    } as unknown as ImageSessionWorkflow;
    let audioState = $state<AuditionState>({ objectId: null, status: 'idle', playheadFrame: 0 });
    let autoplay = $state(false);
    const controller = new AuditionController(transport, (state) => (audioState = state));
    onDestroy(() => controller.dispose());
    const audition = {
        get state() {
            return audioState;
        },
        get autoplay() {
            return autoplay;
        },
        set autoplay(value: boolean) {
            autoplay = value;
        },
        playPrepared: controller.play.bind(controller),
        seekPrepared: controller.seek.bind(controller),
        stop: async () => controller.stop(),
        refreshEditorWorkspace: async (refresh: () => Promise<void>) => refresh(),
    } as unknown as AuditionWorkflow;
    const selection = $derived({
        kind: 'sample',
        item: { objectId: selected },
        preview: {
            preview: {
                lanes: (stereo ? ['left', 'right'] : ['mono']).map((role) => ({
                    role,
                    sampleRate: fixtureRate,
                    storedFrameCount: fixtureFrames,
                    bins: fixtureBins(),
                })),
            },
        },
    } as unknown as InspectorSelection);
</script>

<nav>
    <button onclick={() => (selected = 'Sample A')}>Select A</button><button onclick={() => (selected = 'Sample B')}
        >Select B</button
    ><output aria-label="Write count">{writes}</output>
    <button
        onclick={async () => {
            closed = await imageSession.confirmEditorLeave();
        }}>Close image</button
    >
    <output aria-label="Closed">{String(closed)}</output>
</nav>
{#snippet empty()}{/snippet}
{#snippet content()}<SampleEditorCollection
        {selected}
        {stereo}
        {names}
        {storedFormats}
        revision={writes + 1}
        onselect={(id) => {
            selected = id;
            lowerOpen = true;
        }}
    />{/snippet}
{#snippet editor()}<DeviceEditorHost sessionId={1} {selection} />{/snippet}
<EditorBoundary {transport} {imageSession} {audition}
    >{#if workspace}<div class="workspace-fixture">
            <WorkspaceShell
                mode="device"
                bind:lowerOpen
                imageName="Sample editor fixture"
                imageActions={empty}
                onmodechange={() => {}}
                presentation={{ navigation: empty, content, lower: editor, lowerPreferredHeight: 360 }}
            />
        </div>
    {:else}<main><DeviceEditorHost sessionId={1} {selection} /></main>{/if}</EditorBoundary
>

<style>
    :global(body) {
        margin: 0;
    }
    nav {
        height: 40px;
        display: flex;
        gap: 12px;
        padding: 8px;
    }
    main {
        height: calc(100vh - 40px);
        min-width: 0;
    }
    .workspace-fixture {
        height: calc(100vh - 40px);
    }
    .workspace-fixture :global(.app-shell) {
        height: 100%;
        min-height: 0;
    }
</style>
