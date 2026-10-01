import { afterEach, describe, expect, it, vi } from 'vitest';
import { LogController } from './controller.svelte';
import type { LogDriver, LogPage } from './contracts';

const page = (id = 1): LogPage => ({
    entries: [{ id, source: 'application', level: 'info', timestamp: null, text: `message ${id}` }],
    olderCursor: null,
    newerCursor: null,
    total: 1,
    newCount: 1,
    sequence: id,
    historyChanges: 0,
});
function fixture() {
    vi.useFakeTimers();
    const driver: LogDriver = {
        read: vi.fn().mockResolvedValue(page()),
        clear: vi.fn().mockResolvedValue(8),
        save: vi.fn().mockResolvedValue(null),
    };
    const controller = new LogController(driver);
    return { driver, controller };
}
afterEach(() => vi.useRealTimers());
const settle = async () => {
    for (let i = 0; i < 10; i++) await Promise.resolve();
};

describe('Log viewer controller', () => {
    it('discards a delayed full refresh when selection pauses the displayed page', async () => {
        const { driver, controller } = fixture();
        controller.setActive(true);
        await settle();
        let resolve!: (value: LogPage) => void;
        vi.mocked(driver.read).mockImplementationOnce(
            () =>
                new Promise((done) => {
                    resolve = done;
                }),
        );
        controller.setFollow(true);
        const revision = controller.renderRevision;
        controller.setFollow(false);
        resolve(page(9));
        await settle();
        expect(controller.page?.entries[0].id).toBe(1);
        expect(controller.renderRevision).toBe(revision);
        await vi.advanceTimersByTimeAsync(500);
        expect(vi.mocked(driver.read).mock.lastCall?.[0].metadataOnly).toBe(true);
        controller.setActive(false);
    });

    it('keeps the visible page unchanged while paused and resumes at the latest entry', async () => {
        const { driver, controller } = fixture();
        controller.setActive(true);
        await settle();
        expect(controller.page?.entries[0].id).toBe(1);
        controller.setFollow(false);
        vi.mocked(driver.read).mockResolvedValue(page(2));
        await vi.advanceTimersByTimeAsync(500);
        expect(controller.page?.entries[0].id).toBe(1);
        expect(controller.newCount).toBe(1);
        expect(vi.mocked(driver.read).mock.lastCall?.[0].metadataOnly).toBe(true);
        controller.setFollow(true);
        await settle();
        expect(controller.page?.entries[0].id).toBe(2);
        controller.setActive(false);
    });

    it('clear preserves filters and exports a filtered slice or all retained logs', async () => {
        const { driver, controller } = fixture();
        controller.setActive(true);
        await settle();
        controller.setFilter({ minimumLevel: 'warning', search: 'problem' });
        await settle();
        await controller.clear();
        await settle();
        expect(controller.filter.since).toBe(8);
        expect(controller.filter.minimumLevel).toBe('warning');
        await controller.save(false);
        expect(driver.save).toHaveBeenLastCalledWith(controller.filter);
        await controller.save(true);
        expect(driver.save).toHaveBeenLastCalledWith(null);
        expect(controller.error).toBe('');
        controller.restoreHistory();
        expect(controller.filter.since).toBeNull();
        controller.setActive(false);
    });

    it('does not overlap polling requests or apply stale filter results', async () => {
        const { driver, controller } = fixture();
        let resolve!: (value: LogPage) => void;
        vi.mocked(driver.read).mockImplementationOnce(
            () =>
                new Promise((done) => {
                    resolve = done;
                }),
        );
        controller.setActive(true);
        controller.setFilter({ search: 'new filter' });
        await vi.advanceTimersByTimeAsync(1500);
        expect(driver.read).toHaveBeenCalledTimes(1);
        resolve(page(7));
        await settle();
        expect(driver.read).toHaveBeenCalledTimes(2);
        expect(controller.page?.entries[0].id).toBe(1);
        controller.setActive(false);
    });

    it('stops when hidden, retains the boundary, and catches up on reopening', async () => {
        const { driver, controller } = fixture();
        controller.setActive(true);
        await settle();
        await controller.clear();
        controller.setActive(false);
        await settle();
        vi.mocked(driver.read).mockClear();
        await vi.advanceTimersByTimeAsync(1500);
        expect(driver.read).not.toHaveBeenCalled();
        controller.setActive(true);
        await settle();
        expect(vi.mocked(driver.read).mock.lastCall?.[0].filter.since).toBe(8);
        controller.setActive(false);
    });

    it('keeps errors visible until retry and preserves the previous page', async () => {
        const { driver, controller } = fixture();
        controller.setActive(true);
        await settle();
        vi.mocked(driver.read).mockRejectedValueOnce(new Error('Log not readable'));
        await vi.advanceTimersByTimeAsync(500);
        expect(controller.error).toBe('Log not readable');
        expect(controller.page?.entries[0].id).toBe(1);
        const calls = vi.mocked(driver.read).mock.calls.length;
        await vi.advanceTimersByTimeAsync(1500);
        expect(driver.read).toHaveBeenCalledTimes(calls);
        controller.reload();
        await settle();
        expect(controller.error).toBe('');
        controller.setActive(false);
    });
});
