import { describe, expect, it, vi } from 'vitest';
import { SelectionReadScheduler } from './selectionReadScheduler';

const identity = { imageId: 'image-1', documentId: 'document-1', revision: 1 };

function fixture() {
    const prefetch = vi.fn();
    const cancel = vi.fn();
    const scheduler = new SelectionReadScheduler(prefetch, cancel);
    scheduler.select(1, 'A', identity, true);
    return { scheduler, prefetch, cancel, complete: scheduler.metadataCompletion(1, 'A') };
}

describe('selected read scheduling', () => {
    it('does not cancel or restart a pending prefetch when the same selection is clicked again', () => {
        const f = fixture();
        f.scheduler.prefetch(1, 'A');
        f.scheduler.drawn(1, 'A', identity, { bins: 1024, width: 800, height: 72 });
        f.scheduler.select(1, 'A', identity, false);
        f.scheduler.drawn(1, 'A', identity, { bins: 1024, width: 800, height: 72 });
        expect(f.cancel).toHaveBeenCalledTimes(1);
        expect(f.prefetch).toHaveBeenCalledTimes(1);
    });
    it('starts speculative audio only after a matching nonempty first draw', () => {
        const f = fixture();
        f.scheduler.prefetch(1, 'A');
        f.complete();
        f.scheduler.previewReady(1, 'A', identity);
        expect(f.prefetch).not.toHaveBeenCalled();
        f.scheduler.drawn(1, 'A', identity, { bins: 1024, width: 800, height: 72 });
        expect(f.prefetch).toHaveBeenCalledExactlyOnceWith(1, 'A');
        f.scheduler.drawn(1, 'A', identity, { bins: 2048, width: 1200, height: 72 });
        expect(f.prefetch).toHaveBeenCalledTimes(1);
    });

    it('does not accept blank, stale-object or stale-revision draws', () => {
        const f = fixture();
        f.scheduler.prefetch(1, 'A');
        f.scheduler.drawn(1, 'A', identity, { bins: 0, width: 800, height: 72 });
        f.scheduler.drawn(1, 'B', identity, { bins: 1024, width: 800, height: 72 });
        f.scheduler.drawn(1, 'A', { ...identity, revision: 2 }, { bins: 1024, width: 800, height: 72 });
        expect(f.prefetch).not.toHaveBeenCalled();
    });

    it('yields to playback and discards old deferred selection work', () => {
        const f = fixture();
        const oldMapping = vi.fn();
        f.scheduler.afterForeground(oldMapping);
        f.scheduler.prefetch(1, 'A');
        f.scheduler.playbackPending(true);
        f.scheduler.drawn(1, 'A', identity, { bins: 1024, width: 800, height: 72 });
        expect(f.prefetch).not.toHaveBeenCalled();
        f.scheduler.select(1, 'B', identity, true);
        f.scheduler.playbackPending(false);
        f.complete();
        f.scheduler.previewReady(1, 'A', identity);
        expect(oldMapping).not.toHaveBeenCalled();
        expect(f.prefetch).not.toHaveBeenCalled();
        expect(f.cancel).toHaveBeenCalledTimes(3);
    });

    it('defers Files mapping until both foreground reads finish, without requiring a visible canvas', () => {
        const f = fixture();
        const mapping = vi.fn();
        f.scheduler.afterForeground(mapping);
        f.complete();
        expect(mapping).not.toHaveBeenCalled();
        f.scheduler.previewReady(1, 'A', identity);
        expect(mapping).toHaveBeenCalledTimes(1);
        expect(f.prefetch).not.toHaveBeenCalled();
    });
});
