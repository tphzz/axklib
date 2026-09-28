<script lang="ts">
    import WaveCanvas from '../features/devices/a-series/sample/WaveCanvas.svelte';
    import { ObjectEditorWorkflow } from '../features/object-editor/workflow.svelte';
    import type { ObjectDetail } from '../lib/transport';
    import type { SampleWaveformPreview } from '../lib/types';
    import DeviceEditorHostHarness from './DeviceEditorHostHarness.svelte';
    import { sampleConversionFixture, sampleFormatFixture } from './sampleFormatFixture';
    import { sampleFields } from '../features/devices/a-series/sample/fields';
    import { bankEditorUnits } from './bankEditorFixture';

    const frames = 16384;
    const bins = Array.from({ length: 256 }, () => ({ minimum: -0.25, maximum: 0.25 }));
    const samples = Float32Array.from({ length: frames }, (_, index) => (index % 2 === 0 ? -0.25 : 0.25));
    let native = $state(false);
    let selected = $state('A');
    let waiting = $state(false);
    const pending = new Map<string, (value: ObjectDetail) => void>();
    const names: Record<string, string> = {
        A: 'A',
        B: 'Long Sample Name',
        C: 'C',
        D: 'Envelope Sample',
        E: 'B',
        F: 'Long Bank Name',
    };
    const bank = $derived(selected === 'E' || selected === 'F');

    function detail(id: string): ObjectDetail {
        const bank = id === 'E' || id === 'F';
        const native = ['A', 'C', 'E'].includes(id);
        const format = native ? 'A3000_188' : 'A4000_A5000_224';
        const parameters: Record<string, unknown> = {};
        for (const field of sampleFields) {
            if (field.key.startsWith('playback.')) continue;
            const keys = field.key.split('.');
            let group = parameters;
            for (const key of keys.slice(0, -1)) group = (group[key] ??= {}) as Record<string, unknown>;
            group[keys.at(-1)!] = field.boolean ? false : (field.options?.[0]?.value ?? Math.max(0, field.min));
        }
        return {
            image: { revision: 1 },
            object: { id, key: id, name: names[id], type: bank ? 'SBAC' : 'SBNK' },
            formatConversion: sampleConversionFixture(format),
            editing: {
                profile: bank ? 'a-series/sample-bank' : 'a-series/sample',
                ...(bank ? { bankOverrides: { units: bankEditorUnits(native, new Set()), members: [] } } : {}),
                editable: true,
                reason: '',
                payloadSha256: 'a'.repeat(64),
                parameters: {
                    ...parameters,
                    level: 100,
                    pan: 0,
                    coarse_tune: id === 'A' ? 3 : -2,
                    fine_tune_cents: 0,
                    fixed_pitch: false,
                    random_pitch: 0,
                    loop_mode: 4,
                    loop_start_frame: 0,
                    loop_length_frames: frames,
                },
                playbackWindow: { start_frame: 0, length_frames: frames },
                maximumFrames: frames,
                canEditPlayback: true,
                eqCoefficients: [-15904, 7738, 8192, 15904, -7738],
                blockedParameters: [],
                blockedParameterReasons: {},
                ...sampleFormatFixture(format),
                unavailableParameters: {},
                partitionIndex: 0,
                volumeName: 'Volume',
                sources: [],
            },
        } as unknown as ObjectDetail;
    }
    const workflow = new ObjectEditorWorkflow({
        transport: {
            objectDetail: async (_: number, id: string) => {
                if (id === 'A') return detail(id);
                waiting = true;
                return new Promise<ObjectDetail>((resolve) => pending.set(id, resolve));
            },
            startObjectParameterEdit: async () => {
                throw new Error('Read-only fixture');
            },
            waitForJob: async () => {
                throw new Error('Read-only fixture');
            },
        },
        refresh: async () => {},
        stopPlayback: () => {},
        status: () => {},
    });
    const preview = $derived({
        preview: {
            lanes: ['left', 'right'].map((role) => ({
                role: `${selected} ${role}`,
                sampleRate: 44100,
                storedFrameCount: frames,
                bins,
            })),
        },
    } as unknown as SampleWaveformPreview);
</script>

<nav aria-label="Fixture controls">
    <button onclick={() => (native = !native)} aria-pressed={native}>Native PCM</button>
    <button onclick={() => (selected = 'B')}>Select B</button>
    <button onclick={() => (selected = 'C')}>Select C</button>
    <button onclick={() => (selected = 'D')}>Select D</button>
    <button onclick={() => (selected = 'E')}>Select E</button>
    <button onclick={() => (selected = 'F')}>Select F</button>
    <button
        onclick={() => {
            workflow.comparison.entries = workflow.comparison.count
                ? []
                : [
                      { id: 'A', name: names.A!, values: { level: 100 } },
                      { id: 'D', name: names.D!, error: 'Unavailable comparison fixture' },
                  ];
        }}>Compare</button
    >
    <button onclick={() => workflow.find(1, selected)?.draft.set('level', 99)}>Edit</button>
    <button
        disabled={!waiting}
        onclick={() => {
            pending.get(selected)?.(detail(selected));
            pending.delete(selected);
            waiting = false;
        }}>Complete load</button
    >
</nav>
<div class="canvas-fixture" data-testid="amplitude-plot">
    <WaveCanvas {bins} pcm={native ? samples : undefined} {frames} />
</div>
<div class="overview-fixture" data-testid="amplitude-overview">
    <WaveCanvas {bins} pcm={native ? samples : undefined} {frames} overview />
</div>
<main>
    <DeviceEditorHostHarness {workflow} sample={selected} {preview} kind={bank ? 'sample-bank' : 'sample'} />
</main>

<style>
    :global(body) {
        margin: 0;
    }
    nav {
        display: flex;
        align-items: center;
        gap: 8px;
        flex-wrap: wrap;
        min-height: 40px;
        padding: 4px 8px;
    }
    nav button {
        font-size: 11px;
    }
    .canvas-fixture {
        height: 160px;
    }
    .overview-fixture {
        height: 40px;
    }
    main {
        height: 360px;
        min-width: 0;
    }
</style>
