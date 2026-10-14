import { fireEvent, render, screen } from '@testing-library/svelte';
import { flushSync } from 'svelte';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import type { ObjectDetail } from '../../lib/transport';
import { sampleFormatFixture } from '../../test/sampleFormatFixture';
import type { ImageSessionWorkflow } from '../image-session/workflow.svelte';
import { ObjectEditorDocument, ObjectEditorWorkflow } from '../object-editor/workflow.svelte';
import DeviceDraftRetention from './DeviceDraftRetention.svelte';
import { DeviceDraftStore } from './drafts';

beforeEach(() => {
    vi.restoreAllMocks();
    localStorage.clear();
});
function setup() {
    const editors = new ObjectEditorWorkflow({
        transport: {
            objectDetail: vi.fn(),
            startObjectParameterEdit: vi.fn(),
            waitForJob: vi.fn(),
        },
        refresh: vi.fn(),
        stopPlayback: vi.fn(),
        status: vi.fn(),
    });
    const document = new ObjectEditorDocument(1, {
        formatId: 'yamaha.a-series',
        profile: 'a-series',
        image: { revision: 1 },
        object: { id: 'sample', key: 'sample', name: 'Sample', type: 'SBNK' },
        editing: {
            profile: 'a-series/sample',
            editable: true,
            reason: '',
            payloadSha256: 'a'.repeat(64),
            parameters: { level: 100 },
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
    } as unknown as ObjectDetail);
    editors.documents = [document];
    const images = {
        location: {
            kind: 'device',
            reference: { sessionId: 'owned', targetId: 'disk' },
            displayName: 'Old device',
        },
        setStatus: vi.fn(),
    } as unknown as ImageSessionWorkflow;
    render(DeviceDraftRetention, { editors, images });
    flushSync();
    expect(editors.beforeClear).toBeTypeOf('function');
    return { editors, document, images };
}
describe('device draft lifecycle', () => {
    it('captures the old device before an immediate session clear, without waiting for the debounce', () => {
        const { document, editors, images } = setup();
        document.draft.set('level', 80);
        images.location = null;
        editors.clear();
        const records = new DeviceDraftStore(localStorage).list();
        expect(records).toHaveLength(1);
        expect(records[0]?.device).toBe('Old device');
        expect(records[0]?.values.level).toBe(80);
        expect(editors.documents).toHaveLength(0);
    });
    it('keeps the latest draft exportable after clearing when storage rejects persistence', async () => {
        const { document, editors, images } = setup();
        vi.spyOn(Storage.prototype, 'setItem').mockImplementation(() => {
            throw new Error('Quota exceeded');
        });
        document.draft.set('level', 80);
        images.location = null;
        editors.clear();
        flushSync();
        expect(editors.documents).toHaveLength(0);
        expect(images.setStatus).toHaveBeenCalledWith(expect.stringContaining('held in memory'));
        await fireEvent.click(screen.getByRole('button', { name: 'Retained device drafts (1)' }));
        const select = screen.getByRole('combobox', { name: 'Draft' }) as HTMLSelectElement;
        expect(select.classList.contains('dialog-field-control')).toBe(true);
        await fireEvent.change(select, { target: { value: select.options[1]!.value } });
        expect((screen.getByRole('button', { name: 'Export JSON' }) as HTMLButtonElement).disabled).toBe(false);
    });
});
