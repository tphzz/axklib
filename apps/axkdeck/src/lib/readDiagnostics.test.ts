import { beforeEach, describe, expect, it, vi } from 'vitest';
import type { ImageTransport, PreviewEnvelope } from './transport';
import { beginSampleRead, readContext, readTrace, reportWaveformReadDraw } from './readDiagnostics';

const diagnostics = vi.hoisted(() => ({ enabled: true, report: vi.fn() }));
vi.mock('./diagnostics', () => ({
    diagnosticsEnabled: () => diagnostics.enabled,
    reportDiagnostic: diagnostics.report,
}));
const identity = { imageId: 'image-a', documentId: 'document-a', revision: 1 };
const transport = { audioIdentity: () => identity } as unknown as ImageTransport;

beforeEach(() => {
    diagnostics.enabled = true;
    diagnostics.report.mockClear();
});

describe('sample read diagnostics', () => {
    it('keeps the originating selection on a request that finishes after another selection', () => {
        beginSampleRead(transport, 1, 'sample-a');
        const first = readContext(1, 'sample-a')!;
        const trace = readTrace('preview-http', { objectId: 'sample-a' }, first)!;
        beginSampleRead(transport, 1, 'sample-b');
        trace.mark('headers', { requestId: 'request-1' });
        trace.finish('success');
        const [, event] = diagnostics.report.mock.calls.at(-1)!;
        expect(event).toMatchObject({ selectionId: first.selectionId, requestId: 'request-1', current: false });
        expect(readContext(1, 'sample-a')).toBeUndefined();
        expect(readContext(2, 'sample-b')).toBeUndefined();
    });

    it('records only nonempty current waveform draws and refinements, not stale revisions', () => {
        beginSampleRead(transport, 1, 'sample-a');
        const envelope = { ...identity, objectId: 'sample-a' } as PreviewEnvelope;
        const draw = (bins: number, overrides = {}) =>
            reportWaveformReadDraw(1, 'sample-a', envelope, {
                bins,
                width: 800,
                height: 100,
                ...overrides,
            });
        draw(0);
        draw(1024, { width: 1 });
        draw(1024);
        draw(1024);
        draw(2048);
        reportWaveformReadDraw(1, 'sample-a', { ...envelope, revision: 2 }, { bins: 4096, width: 800, height: 100 });
        beginSampleRead(transport, 1, 'sample-b');
        draw(4096);
        const phases = diagnostics.report.mock.calls.map(([, event]) => event.phase);
        expect(phases.filter((phase) => phase === 'waveform-first-draw')).toHaveLength(1);
        expect(phases.filter((phase) => phase === 'waveform-refinement-draw')).toHaveLength(1);
    });

    it('does no identity lookup or logging while capture is disabled, even if the sink fails', () => {
        diagnostics.enabled = false;
        const audioIdentity = vi.fn(() => {
            throw new Error('closed session');
        });
        beginSampleRead({ audioIdentity } as unknown as ImageTransport, 1, 'sample-a');
        expect(audioIdentity).not.toHaveBeenCalled();
        expect(readTrace('preview-http')).toBeUndefined();
        expect(diagnostics.report).not.toHaveBeenCalled();
        diagnostics.enabled = true;
        diagnostics.report.mockImplementationOnce(() => {
            throw new Error('log failure');
        });
        expect(() => beginSampleRead(transport, 1, 'sample-a')).not.toThrow();
    });
});
