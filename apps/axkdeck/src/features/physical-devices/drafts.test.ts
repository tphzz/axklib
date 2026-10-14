import { beforeEach, describe, expect, it, vi } from 'vitest';
import { ObjectEditorDocument } from '../object-editor/workflow.svelte';
import { DeviceDraftStore } from './drafts';
import type { ObjectDetail } from '../../lib/transport';
import { sampleFormatFixture } from '../../test/sampleFormatFixture';
const location = {
    kind: 'device' as const,
    reference: { sessionId: 'owned', targetId: 'disk' },
    displayName: 'Test / serial / Disk',
};
function document(hash = 'a'.repeat(64)) {
    const detail = {
        formatId: 'yamaha.a-series',
        profile: 'a-series',
        image: { revision: 1 },
        object: { id: 'sample', key: 'sample', name: 'Sample', type: 'SBNK' },
        editing: {
            profile: 'a-series/sample',
            editable: true,
            reason: '',
            payloadSha256: hash,
            parameters: { level: 100, pan: 0 },
            playbackWindow: { start_frame: 0, length_frames: 100 },
            maximumFrames: 100,
            canEditPlayback: true,
            ...sampleFormatFixture(),
            blockedParameters: [],
            blockedParameterReasons: {},
            unavailableParameters: {},
            partitionIndex: 0,
            volumeName: 'Volume',
            sources: [],
        },
    } as unknown as ObjectDetail;
    return new ObjectEditorDocument(1, detail);
}
beforeEach(() => localStorage.clear());
describe('retained device parameter drafts', () => {
    it('retains drafts across session closure and restores only after explicit review', () => {
        const old = document();
        old.draft.set('level', 80);
        const store = new DeviceDraftStore(localStorage);
        store.sync(location, [old]);
        store.sync(location, []);
        const restarted = new DeviceDraftStore(localStorage);
        const current = document();
        restarted.sync(location, [current]);
        expect(current.draft.values.level).toBe(100);
        expect(restarted.list()).toHaveLength(1);
        restarted.restore(restarted.list()[0]!, location, [current]);
        expect(current.draft.values.level).toBe(80);
        expect(current.draft.dirty).toBe(true);
    });
    it('refuses a different base payload or device and never silently replaces current edits', () => {
        const old = document();
        old.draft.set('level', 80);
        const store = new DeviceDraftStore(localStorage);
        store.sync(location, [old]);
        const record = store.list()[0]!;
        expect(() => store.restore(record, location, [document('b'.repeat(64))])).toThrow('unchanged base');
        expect(() => store.restore(record, { ...location, displayName: 'Other device' }, [document()])).toThrow(
            'unchanged base',
        );
        const edited = document();
        edited.draft.set('level', 90);
        expect(() => store.restore(record, location, [edited])).toThrow('clean');
        expect(edited.draft.values.level).toBe(90);
    });
    it('removes a captured draft only after an explicit discard or confirmed clean save', () => {
        const current = document();
        current.draft.set('level', 80);
        const store = new DeviceDraftStore(localStorage);
        store.sync(location, [current]);
        current.draft.discard();
        store.sync(location, [current]);
        expect(store.list()).toHaveLength(0);
    });
    it('fails closed for malformed retained data', () => {
        localStorage.setItem('axkdeck.device-drafts', '{}');
        expect(() => new DeviceDraftStore(localStorage)).toThrow('invalid');
    });
    it('keeps rejected writes in memory and retries persistence without losing the latest values', () => {
        const current = document();
        current.draft.set('level', 80);
        const storage = {
            getItem: () => null,
            setItem: vi.fn().mockImplementationOnce(() => {
                throw new Error('Quota exceeded');
            }),
        } as unknown as Storage;
        const store = new DeviceDraftStore(storage);
        expect(() => store.sync(location, [current])).toThrow('Quota exceeded');
        expect(store.list()[0]?.values.level).toBe(80);
        store.sync(location, []);
        expect(storage.setItem).toHaveBeenCalledTimes(2);
        expect(JSON.parse(vi.mocked(storage.setItem).mock.calls[1]![1])[0].values.level).toBe(80);
    });
    it('keeps new drafts exportable when persistent storage could not be opened', () => {
        const current = document();
        current.draft.set('level', 80);
        const store = new DeviceDraftStore(null);
        expect(() => store.sync(location, [current])).toThrow('unavailable');
        expect(store.list()[0]?.values.level).toBe(80);
    });
    it('rejects an identity changed independently of its stored base', () => {
        const old = document();
        old.draft.set('level', 80);
        const store = new DeviceDraftStore(localStorage);
        store.sync(location, [old]);
        const record = store.list()[0]!;
        record.payloadSha256 = 'b'.repeat(64);
        expect(() => store.restore(record, location, [document()])).toThrow('identity');
    });
});
