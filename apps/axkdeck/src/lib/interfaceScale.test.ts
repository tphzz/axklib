import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import {
    automaticInterfaceZoom,
    createInterfaceScaleController,
    type InterfaceScaleAdapter,
    type InterfaceScaleMonitor,
    type InterfaceScaleMode,
} from './interfaceScale';

function monitor(width: number, height: number, scaleFactor = 1, name = 'Display'): InterfaceScaleMonitor {
    return { name, physicalWidth: width, physicalHeight: height, scaleFactor };
}

function adapterFor(initialMonitor: InterfaceScaleMonitor | null = monitor(1920, 1080)): {
    adapter: InterfaceScaleAdapter;
    setMonitor: (value: InterfaceScaleMonitor | null) => void;
    emitMoved: () => void;
    emitScaleChanged: () => void;
} {
    let current = initialMonitor;
    let moved: (() => void) | undefined;
    let scaleChanged: (() => void) | undefined;
    const adapter: InterfaceScaleAdapter = {
        currentMonitor: vi.fn(async () => current),
        windowScaleFactor: vi.fn(async () => current?.scaleFactor ?? 1),
        innerSize: vi.fn(async () => ({ width: 1440, height: 900 })),
        setMinSize: vi.fn(async () => undefined),
        setSize: vi.fn(async () => undefined),
        setZoom: vi.fn(async () => undefined),
        onMoved: vi.fn(async (handler) => {
            moved = handler;
            return () => {
                moved = undefined;
            };
        }),
        onScaleChanged: vi.fn(async (handler) => {
            scaleChanged = handler;
            return () => {
                scaleChanged = undefined;
            };
        }),
    };
    return {
        adapter,
        setMonitor: (value) => {
            current = value;
        },
        emitMoved: () => moved?.(),
        emitScaleChanged: () => scaleChanged?.(),
    };
}

function storageWith(value?: string): Storage {
    const values = new Map<string, string>();
    if (value !== undefined) values.set('axkdeck.interface-scale.v1', value);
    return {
        get length() {
            return values.size;
        },
        clear: () => values.clear(),
        getItem: (key) => values.get(key) ?? null,
        key: (index) => [...values.keys()][index] ?? null,
        removeItem: (key) => {
            values.delete(key);
        },
        setItem: (key, next) => {
            values.set(key, next);
        },
    };
}

describe('automaticInterfaceZoom', () => {
    it.each([
        [monitor(1920, 1080), 1],
        [monitor(2560, 1440), 1.25],
        [monitor(3440, 1440), 1.25],
        [monitor(3840, 2160), 1.5],
        [monitor(2160, 3840), 1.5],
        [monitor(3840, 2160, 1.25), 1.2],
        [monitor(3840, 2160, 1.5), 1],
        [monitor(5120, 2880, 2), 1],
    ])('calculates a bounded OS-aware zoom for %#', (display, expected) => {
        expect(automaticInterfaceZoom(display)).toBe(expected);
    });

    it('uses the neutral zoom for missing or invalid monitor data', () => {
        expect(automaticInterfaceZoom(null)).toBe(1);
        expect(automaticInterfaceZoom(monitor(3840, 2160, 0))).toBe(1);
        expect(automaticInterfaceZoom(monitor(Number.NaN, 2160))).toBe(1);
    });
});

describe('InterfaceScaleController', () => {
    beforeEach(() => {
        vi.useFakeTimers();
    });

    afterEach(() => {
        vi.useRealTimers();
    });

    it('applies automatic scaling and scaled minimum window constraints before returning', async () => {
        const { adapter } = adapterFor(monitor(3840, 2160));

        const controller = await createInterfaceScaleController(adapter, storageWith());

        expect(controller.state()).toEqual({ mode: 'auto', appliedZoom: 1.5 });
        expect(adapter.setMinSize).toHaveBeenCalledWith(1200, 900);
        expect(adapter.setSize).not.toHaveBeenCalled();
        expect(adapter.setZoom).toHaveBeenCalledWith(1.5);
        await controller.dispose();
    });

    it('expands an undersized window before applying zoom', async () => {
        const { adapter } = adapterFor(monitor(3840, 2160));
        vi.mocked(adapter.innerSize).mockResolvedValue({ width: 1000, height: 700 });

        const controller = await createInterfaceScaleController(adapter, storageWith());

        expect(adapter.setSize).toHaveBeenCalledWith(1200, 900);
        expect(vi.mocked(adapter.setSize).mock.invocationCallOrder[0]).toBeLessThan(
            vi.mocked(adapter.setZoom).mock.invocationCallOrder[0]!,
        );
        await controller.dispose();
    });

    it('loads fixed persisted modes and ignores monitor changes until Auto is selected', async () => {
        const { adapter, emitScaleChanged, setMonitor } = adapterFor(monitor(1920, 1080));
        const storage = storageWith('1.25');
        const controller = await createInterfaceScaleController(adapter, storage);

        expect(controller.state()).toEqual({ mode: '1.25', appliedZoom: 1.25 });
        setMonitor(monitor(3840, 2160));
        emitScaleChanged();
        await vi.runAllTimersAsync();
        expect(controller.state()).toEqual({ mode: '1.25', appliedZoom: 1.25 });

        await controller.setMode('auto');
        expect(storage.getItem('axkdeck.interface-scale.v1')).toBe('auto');
        expect(controller.state()).toEqual({ mode: 'auto', appliedZoom: 1.5 });
        await controller.dispose();
    });

    it.each(['', '125%', '2', 'invalid'])('falls back to Auto for an invalid stored mode %j', async (stored) => {
        const { adapter } = adapterFor(monitor(2560, 1440));

        const controller = await createInterfaceScaleController(adapter, storageWith(stored));

        expect(controller.state()).toEqual({ mode: 'auto', appliedZoom: 1.25 });
        await controller.dispose();
    });

    it('continues when scale preferences cannot be read or written', async () => {
        const { adapter } = adapterFor(monitor(3840, 2160));
        const report = vi.fn();
        const storage = {
            getItem: vi.fn(() => {
                throw new Error('read failed');
            }),
            setItem: vi.fn(() => {
                throw new Error('write failed');
            }),
            removeItem: vi.fn(),
            clear: vi.fn(),
            key: vi.fn(),
            length: 0,
        } satisfies Storage;

        const controller = await createInterfaceScaleController(adapter, storage, report);
        await controller.setMode('1.25');

        expect(controller.state()).toEqual({ mode: '1.25', appliedZoom: 1.25 });
        expect(report).toHaveBeenCalledWith(
            'interface_scale_storage_failed',
            expect.objectContaining({ message: 'read failed' }),
            'warn',
        );
        expect(report).toHaveBeenCalledWith(
            'interface_scale_storage_failed',
            expect.objectContaining({ message: 'write failed' }),
            'warn',
        );
        await controller.dispose();
    });

    it('debounces monitor movement, reacts immediately to scale changes, and suppresses duplicate zoom calls', async () => {
        const { adapter, emitMoved, emitScaleChanged, setMonitor } = adapterFor(monitor(1920, 1080));
        const controller = await createInterfaceScaleController(adapter, storageWith());
        vi.mocked(adapter.setZoom).mockClear();

        emitMoved();
        emitMoved();
        await vi.advanceTimersByTimeAsync(149);
        expect(adapter.currentMonitor).toHaveBeenCalledTimes(1);
        await vi.advanceTimersByTimeAsync(1);
        expect(adapter.currentMonitor).toHaveBeenCalledTimes(2);
        expect(adapter.setZoom).not.toHaveBeenCalled();

        setMonitor(monitor(3840, 2160));
        emitScaleChanged();
        await vi.runAllTimersAsync();
        expect(adapter.setZoom).toHaveBeenCalledOnce();
        expect(controller.state().appliedZoom).toBe(1.5);
        await controller.dispose();
    });

    it('notifies subscribers, persists valid modes, and removes listeners on disposal', async () => {
        const { adapter, emitMoved, emitScaleChanged } = adapterFor(monitor(1920, 1080));
        const storage = storageWith();
        const controller = await createInterfaceScaleController(adapter, storage);
        const listener = vi.fn();
        const unsubscribe = controller.subscribe(listener);

        await controller.setMode('1.5');

        expect(storage.getItem('axkdeck.interface-scale.v1')).toBe('1.5');
        expect(listener).toHaveBeenLastCalledWith({ mode: '1.5', appliedZoom: 1.5 });
        unsubscribe();
        await controller.dispose();
        emitMoved();
        emitScaleChanged();
        await vi.runAllTimersAsync();
        expect(adapter.currentMonitor).toHaveBeenCalledTimes(2);
    });

    it('reports zoom failures without preventing startup', async () => {
        const { adapter } = adapterFor(monitor(3840, 2160));
        vi.mocked(adapter.setZoom).mockRejectedValue(new Error('zoom unavailable'));
        const report = vi.fn();

        const controller = await createInterfaceScaleController(adapter, storageWith(), report);

        expect(controller.state()).toEqual({ mode: 'auto', appliedZoom: 1 });
        expect(report).toHaveBeenCalledWith(
            'interface_scale_apply_failed',
            expect.objectContaining({ message: 'zoom unavailable' }),
            'warn',
        );
        await controller.dispose();
    });

    it('accepts every declared fixed mode', async () => {
        const { adapter } = adapterFor();
        const controller = await createInterfaceScaleController(adapter, storageWith());
        const modes: InterfaceScaleMode[] = ['1', '1.25', '1.5', 'auto'];

        for (const mode of modes) await controller.setMode(mode);

        expect(controller.state().mode).toBe('auto');
        await controller.dispose();
    });
});
