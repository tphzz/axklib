import { sampleFormatFixture } from '../../test/sampleFormatFixture';
import { waitFor } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import type { ObjectDetail } from '../../lib/transport';
import { EditorComparison } from './comparison.svelte';
import { ObjectEditorDocument } from './workflow.svelte';

function detail(id: string, parameters: Record<string, unknown> = { level: 100 }): ObjectDetail {
    return {
        image: { revision: 1 },
        object: { id, key: id, name: `Sample ${id}` },
        editing: {
            profile: 'a-series/sample',
            editable: true,
            reason: '',
            payloadSha256: 'a'.repeat(64),
            parameters,
            playbackWindow: { start_frame: 0, length_frames: 100 },
            maximumFrames: 100,
            canEditPlayback: true,
            eqCoefficients: [-15904, 7738, 8192, 15904, -7738],
            blockedParameters: [],
            blockedParameterReasons: {},
            ...sampleFormatFixture(),
            unavailableParameters: {},
            partitionIndex: 0,
            volumeName: 'Volume',
            sources: [],
        },
    } as unknown as ObjectDetail;
}

function deferred<T>() {
    let resolve!: (value: T) => void;
    let reject!: (reason: Error) => void;
    const promise = new Promise<T>((done, fail) => {
        resolve = done;
        reject = fail;
    });
    return { promise, resolve, reject };
}

describe('Sample parameter comparison', () => {
    it('compares numbers, booleans, nested fields, and unavailable values without conflating false with zero', async () => {
        const samples = {
            A: detail('A', { level: 100, fixed_pitch: false, pan: 0, lfo: { speed: 40, delay: 0 } }),
            B: detail('B', { level: 100, fixed_pitch: true, pan: false, lfo: { speed: 40 } }),
        };
        const transport = { objectDetail: vi.fn(async (_: number, id: string) => samples[id as keyof typeof samples]) };
        const comparison = new EditorComparison(transport, () => undefined);

        await comparison.select(1, 1, ['A', 'B']);

        expect(comparison.count).toBe(2);
        expect(comparison.differing('level')).toBe(false);
        expect(comparison.differing('fixed_pitch')).toBe(true);
        expect(comparison.differing('pan')).toBe(true);
        expect(comparison.differing('lfo.speed')).toBe(false);
        expect(comparison.differing('lfo.delay')).toBe(true);
        expect(comparison.differing('portamento_rate')).toBe(false);
        expect(comparison.description('lfo.delay')).toContain('Sample A');
        expect(comparison.description('lfo.delay')).toContain('Sample B');
        expect(comparison.description('lfo.delay')).toMatch(/unavailable/i);
        const formatted = comparison.description('fixed_pitch', (value) => (value ? 'Enabled' : 'Disabled'));
        expect(formatted).toContain('Enabled');
        expect(formatted).toContain('Disabled');
    });

    it('uses the current draft, including undo and discard, instead of its cached server snapshot', async () => {
        const a = new ObjectEditorDocument(1, detail('A'));
        const documents = new Map<string, ObjectEditorDocument>([['A', a]]);
        const transport = { objectDetail: vi.fn(async (_: number, id: string) => detail(id)) };
        const comparison = new EditorComparison(transport, (session, id) =>
            session === 1 ? documents.get(id) : undefined,
        );
        await comparison.select(1, 1, ['A', 'B']);
        expect(comparison.differing('level')).toBe(false);

        a.draft.set('level', 64);
        expect(comparison.differing('level')).toBe(true);
        expect(comparison.description('level')).toContain('64');
        a.draft.undo();
        expect(comparison.differing('level')).toBe(false);
        a.draft.redo();
        expect(comparison.differing('level')).toBe(true);
        a.draft.discard();
        expect(comparison.differing('level')).toBe(false);

        const b = new ObjectEditorDocument(1, detail('B'));
        b.draft.set('level', 32);
        documents.set('B', b);
        expect(comparison.differing('level')).toBe(true);
        expect(comparison.description('level')).toContain('32');
        expect(a.draft.values.level).toBe(100);
        expect(b.draft.values.level).toBe(32);
    });

    it('keeps failed and unsupported Samples explicit instead of describing incomplete results as matches', async () => {
        const transport = {
            objectDetail: vi.fn(async (_: number, id: string) => {
                if (id === 'B') throw new Error('Connection lost');
                if (id === 'C') return { ...detail(id), editing: undefined };
                return detail(id);
            }),
        };
        const comparison = new EditorComparison(transport, () => undefined);

        await comparison.select(1, 1, ['A', 'B', 'C']);

        expect(comparison.count).toBe(3);
        expect(comparison.entries.find((entry) => entry.id === 'B')?.error).toBeTruthy();
        expect(comparison.entries.find((entry) => entry.id === 'C')?.error).toBeTruthy();
        expect(comparison.status).toMatch(/incomplete|failed|unavailable|error/i);
        expect(comparison.description('level')).toMatch(/connection lost|failed|unavailable|error/i);
        expect(comparison.description('level')).not.toMatch(/all (?:values|samples) match/i);
    });

    it('marks pending comparisons as loading rather than treating absent snapshots as matching values', async () => {
        const response = deferred<ObjectDetail>();
        const comparison = new EditorComparison({ objectDetail: () => response.promise }, () => undefined);

        const loading = comparison.select(1, 1, ['A', 'B']);
        expect(comparison.count).toBe(2);
        expect(comparison.status).toMatch(/loading|comparing/i);
        expect(comparison.description('level')).toMatch(/loading|pending/i);
        response.resolve(detail('A'));
        await loading;
    });

    it('ignores responses from an older selection even when they arrive last', async () => {
        const responses = new Map(['A', 'B', 'C', 'D'].map((id) => [id, deferred<ObjectDetail>()]));
        const transport = {
            objectDetail: vi.fn((_: number, id: string) => responses.get(id)!.promise),
        };
        const comparison = new EditorComparison(transport, () => undefined);
        const previous = comparison.select(1, 1, ['A', 'B']);
        const current = comparison.select(1, 1, ['C', 'D']);
        responses.get('C')!.resolve(detail('C', { level: 50 }));
        responses.get('D')!.resolve(detail('D', { level: 50 }));
        await current;
        expect(comparison.entries.map((entry) => entry.id)).toEqual(['C', 'D']);

        responses.get('A')!.resolve(detail('A', { level: 0 }));
        responses.get('B')!.resolve(detail('B', { level: 127 }));
        await previous;
        expect(comparison.entries.map((entry) => entry.id)).toEqual(['C', 'D']);
        expect(comparison.differing('level')).toBe(false);
        expect(comparison.description('level')).not.toContain('Sample A');
    });

    it('clears an in-flight comparison without allowing late results to restore it', async () => {
        const response = deferred<ObjectDetail>();
        const comparison = new EditorComparison({ objectDetail: () => response.promise }, () => undefined);
        const loading = comparison.select(1, 1, ['A', 'B']);

        comparison.clear();
        response.resolve(detail('A'));
        await loading;

        expect(comparison.entries).toEqual([]);
        expect(comparison.count).toBe(0);
        expect(comparison.status).toBe('');
        expect(comparison.differing('level')).toBe(false);
        expect(comparison.description('level')).toBe('');
    });

    it('reuses snapshots only within the same image session and revision', async () => {
        let level = 100;
        const transport = {
            objectDetail: vi.fn(async (_: number, id: string) => detail(id, { level: id === 'A' ? 100 : level })),
        };
        const comparison = new EditorComparison(transport, () => undefined);
        await comparison.select(1, 1, ['A', 'B']);
        await comparison.select(1, 1, ['B', 'A']);
        expect(transport.objectDetail).toHaveBeenCalledTimes(2);
        expect(comparison.differing('level')).toBe(false);

        level = 64;
        await comparison.select(1, 2, ['A', 'B']);
        expect(transport.objectDetail).toHaveBeenCalledTimes(4);
        expect(comparison.differing('level')).toBe(true);
        expect(comparison.description('level')).toContain('64');

        level = 100;
        await comparison.select(2, 2, ['A', 'B']);
        expect(transport.objectDetail).toHaveBeenCalledTimes(6);
        expect(comparison.differing('level')).toBe(false);
    });

    it('bounds snapshot loading to four concurrent requests for a large selection', async () => {
        let active = 0;
        let maximum = 0;
        const responses = new Map<string, ReturnType<typeof deferred<ObjectDetail>>>();
        const transport = {
            objectDetail: vi.fn(async (_: number, id: string) => {
                active++;
                maximum = Math.max(maximum, active);
                const response = deferred<ObjectDetail>();
                responses.set(id, response);
                try {
                    return await response.promise;
                } finally {
                    active--;
                }
            }),
        };
        const comparison = new EditorComparison(transport, () => undefined);
        const ids = Array.from({ length: 9 }, (_, index) => String(index));
        const loading = comparison.select(1, 1, ids);

        await waitFor(() => expect(responses.size).toBe(4));
        for (const [id, response] of responses) response.resolve(detail(id));
        await waitFor(() => expect(responses.size).toBe(8));
        for (const [id, response] of responses) response.resolve(detail(id));
        await waitFor(() => expect(responses.size).toBe(9));
        responses.get('8')!.resolve(detail('8'));
        await loading;

        expect(maximum).toBeLessThanOrEqual(4);
        expect(active).toBe(0);
        expect(comparison.count).toBe(9);
        expect(comparison.entries.every((entry) => entry.values && !entry.error)).toBe(true);
        expect(comparison.differing('level')).toBe(false);
    });
});
