import { describe, expect, it, vi } from 'vitest';
import { defaultLogFilter, includesLevel, logLevels, logSources } from './contracts';
import { LogController } from './controller.svelte';

describe('runtime diagnostic capture', () => {
    it('does not restore elevated capture after a close event overtakes its acknowledgement', async () => {
        let resolve!: (state: import('./contracts').CaptureState) => void;
        const controller = new LogController({
            read: vi.fn(),
            clear: vi.fn(),
            save: vi.fn(),
            capture: vi.fn(),
            setCapture: () =>
                new Promise((done) => {
                    resolve = done;
                }),
        });
        const pending = controller.setLevel('trace');
        controller.updateCapture({ level: 'info', localServer: 'ready', error: null });
        resolve({ level: 'trace', localServer: 'ready', error: null });
        await pending;
        expect(controller.capture.level).toBe('info');
        expect(controller.filter.level).toBe('info');
        expect(controller.captureBusy).toBe(false);
    });

    it('ignores a stale initial read after a runtime capture event', async () => {
        let resolve!: (state: import('./contracts').CaptureState) => void;
        const controller = new LogController({
            read: vi.fn(),
            clear: vi.fn(),
            save: vi.fn(),
            setCapture: vi.fn(),
            capture: () =>
                new Promise((done) => {
                    resolve = done;
                }),
        });
        const pending = controller.readCapture();
        controller.updateCapture({ level: 'debug', localServer: 'ready', error: null });
        resolve({ level: 'info', localServer: 'unavailable', error: null });
        await pending;
        expect(controller.capture.level).toBe('debug');
    });
    it('defaults to Info and uses the cumulative five-level ordering', () => {
        expect(defaultLogFilter()).toMatchObject({
            level: 'info',
            sources: [...logSources],
        });
        for (const [index, limit] of logLevels.entries()) {
            expect(logLevels.filter((level) => includesLevel(limit, level))).toEqual(logLevels.slice(0, index + 1));
        }
    });

    it('changes capture and display together without changing sources or search', async () => {
        const setCapture = vi.fn().mockResolvedValue({ level: 'trace', localServer: 'ready', error: null });
        const controller = new LogController({
            read: vi.fn(),
            clear: vi.fn(),
            save: vi.fn(),
            capture: vi.fn(),
            setCapture,
        });
        controller.setFilter({ sources: ['objects'], search: 'save' });
        await controller.setLevel('trace');
        expect(controller.capture.level).toBe('trace');
        expect(controller.filter.level).toBe('trace');
        expect(controller.filter.sources).toEqual(['objects']);
        expect(controller.filter.search).toBe('save');
        setCapture.mockRejectedValueOnce(new Error('server unavailable'));
        await controller.setLevel('info');
        expect(controller.capture.level).toBe('trace');
        expect(controller.filter.level).toBe('trace');
        expect(controller.captureError).toBe('server unavailable');
    });

    it('keeps the confirmed level during a pending update and supports lowering it', async () => {
        let resolve!: (state: import('./contracts').CaptureState) => void;
        const controller = new LogController({
            read: vi.fn(),
            clear: vi.fn(),
            save: vi.fn(),
            capture: vi.fn(),
            setCapture: () =>
                new Promise((done) => {
                    resolve = done;
                }),
        });
        controller.updateCapture({ level: 'trace', localServer: 'ready', error: null });
        controller.setFilter({ sources: ['devices'], search: 'rename', since: 12 });
        const pending = controller.setLevel('error');
        expect(controller.filter.level).toBe('trace');
        resolve({ level: 'error', localServer: 'ready', error: null });
        await pending;
        expect(controller.filter).toMatchObject({ level: 'error', sources: ['devices'], search: 'rename', since: 12 });
        controller.updateCapture({ level: 'info', localServer: 'ready', error: null });
        expect(controller.filter.level).toBe('info');
    });
});
