export type InterfaceScaleMode = 'auto' | '1' | '1.15' | '1.25' | '1.5';

export interface InterfaceScaleState {
    mode: InterfaceScaleMode;
    appliedZoom: number;
}

export interface InterfaceScaleMonitor {
    name: string | null;
    physicalWidth: number;
    physicalHeight: number;
    scaleFactor: number;
}

interface PhysicalSize {
    width: number;
    height: number;
}

type Unlisten = () => void;
type ScaleReporter = (
    event: string,
    fields: Readonly<Record<string, unknown>>,
    level?: 'info' | 'warn' | 'error',
) => void;
type PersistScaleMode = (mode: InterfaceScaleMode) => Promise<void>;

export interface InterfaceScaleAdapter {
    currentMonitor(): Promise<InterfaceScaleMonitor | null>;
    windowScaleFactor(): Promise<number>;
    innerSize(): Promise<PhysicalSize>;
    isMaximized(): Promise<boolean>;
    isFullscreen(): Promise<boolean>;
    setMinSize(width: number, height: number): Promise<void>;
    setSize(width: number, height: number): Promise<void>;
    setZoom(zoom: number): Promise<void>;
    onMoved(handler: () => void): Promise<Unlisten>;
    onResized(handler: () => void): Promise<Unlisten>;
    onScaleChanged(handler: () => void): Promise<Unlisten>;
}

export interface InterfaceScaleController {
    state(): InterfaceScaleState;
    setMode(mode: InterfaceScaleMode): Promise<void>;
    subscribe(listener: (state: InterfaceScaleState) => void): Unlisten;
    dispose(): Promise<void>;
}

const minimumWindowWidth = 800;
const minimumWindowHeight = 600;
const windowEventDebounceMs = 150;

function finitePositive(value: number): boolean {
    return Number.isFinite(value) && value > 0;
}

function roundedZoom(value: number): number {
    return Math.round(value * 100) / 100;
}

function requestedScaleForResolution(shorterDimension: number): number {
    if (shorterDimension >= 2000) return 1.5;
    if (shorterDimension >= 1400) return 1.25;
    return 1;
}

export function automaticInterfaceZoom(monitor: InterfaceScaleMonitor | null): number {
    if (
        !monitor ||
        !finitePositive(monitor.physicalWidth) ||
        !finitePositive(monitor.physicalHeight) ||
        !finitePositive(monitor.scaleFactor)
    ) {
        return 1;
    }
    const target = requestedScaleForResolution(Math.min(monitor.physicalWidth, monitor.physicalHeight));
    return roundedZoom(Math.max(1, Math.min(1.5, target / monitor.scaleFactor)));
}

function fixedZoom(mode: InterfaceScaleMode): number | null {
    if (mode === 'auto') return null;
    return Number(mode);
}

function errorMessage(error: unknown): string {
    return error instanceof Error ? error.message : String(error);
}

export async function createInterfaceScaleController(
    adapter: InterfaceScaleAdapter,
    initialMode: InterfaceScaleMode,
    persistMode: PersistScaleMode = async () => undefined,
    report: ScaleReporter = () => undefined,
): Promise<InterfaceScaleController> {
    let current: InterfaceScaleState = {
        mode: initialMode,
        appliedZoom: 1,
    };
    const listeners = new Set<(state: InterfaceScaleState) => void>();
    const unlisteners: Unlisten[] = [];
    let disposed = false;
    let windowEventTimer: ReturnType<typeof setTimeout> | undefined;
    let updateQueue = Promise.resolve();
    let constrainedZoom: number | null = null;

    const notify = (): void => {
        const snapshot = { ...current };
        for (const listener of listeners) listener(snapshot);
    };

    const apply = async (): Promise<void> => {
        if (disposed) return;
        let monitor: InterfaceScaleMonitor | null = null;
        try {
            monitor = await adapter.currentMonitor();
        } catch (error) {
            if (current.mode === 'auto') throw error;
        }
        const monitorFactor = monitor?.scaleFactor;
        const scaleFactor =
            monitorFactor !== undefined && finitePositive(monitorFactor)
                ? monitorFactor
                : await adapter.windowScaleFactor();
        const normalizedMonitor = monitor ? { ...monitor, scaleFactor } : null;
        const requestedZoom = fixedZoom(current.mode) ?? automaticInterfaceZoom(normalizedMonitor);
        const zoomChanged = current.appliedZoom !== requestedZoom;
        const constraintsChanged = constrainedZoom !== requestedZoom;
        if (!zoomChanged && !constraintsChanged) return;

        if (constraintsChanged) {
            try {
                const [maximized, fullscreen] = await Promise.all([adapter.isMaximized(), adapter.isFullscreen()]);
                if (!maximized && !fullscreen) {
                    const minimumWidth = Math.round(minimumWindowWidth * requestedZoom);
                    const minimumHeight = Math.round(minimumWindowHeight * requestedZoom);
                    await adapter.setMinSize(minimumWidth, minimumHeight);
                    const physicalSize = await adapter.innerSize();
                    const factor = finitePositive(scaleFactor) ? scaleFactor : 1;
                    const logicalWidth = physicalSize.width / factor;
                    const logicalHeight = physicalSize.height / factor;
                    if (logicalWidth < minimumWidth || logicalHeight < minimumHeight) {
                        await adapter.setSize(
                            Math.max(logicalWidth, minimumWidth),
                            Math.max(logicalHeight, minimumHeight),
                        );
                    }
                    constrainedZoom = requestedZoom;
                }
            } catch (error) {
                report(
                    'interface_scale_window_constraint_failed',
                    { message: errorMessage(error), requestedZoom },
                    'warn',
                );
            }
        }

        if (!zoomChanged) return;
        await adapter.setZoom(requestedZoom);
        current = { ...current, appliedZoom: requestedZoom };
        notify();
        report('interface_scale_applied', {
            mode: current.mode,
            zoom: requestedZoom,
            monitor: monitor?.name ?? null,
            physicalWidth: monitor?.physicalWidth ?? null,
            physicalHeight: monitor?.physicalHeight ?? null,
            osScaleFactor: finitePositive(scaleFactor) ? scaleFactor : null,
        });
    };

    const queueApply = (): Promise<void> => {
        updateQueue = updateQueue
            .then(apply)
            .catch((error) =>
                report('interface_scale_apply_failed', { message: errorMessage(error), mode: current.mode }, 'warn'),
            );
        return updateQueue;
    };

    const registerListener = async (registration: Promise<Unlisten>, event: string): Promise<void> => {
        try {
            const unlisten = await registration;
            if (disposed) unlisten();
            else unlisteners.push(unlisten);
        } catch (error) {
            report(event, { message: errorMessage(error) }, 'warn');
        }
    };

    const scheduleApply = (): void => {
        if (windowEventTimer !== undefined) clearTimeout(windowEventTimer);
        windowEventTimer = setTimeout(() => {
            windowEventTimer = undefined;
            void queueApply();
        }, windowEventDebounceMs);
    };

    await Promise.all([
        registerListener(
            adapter.onScaleChanged(() => {
                if (windowEventTimer !== undefined) {
                    clearTimeout(windowEventTimer);
                    windowEventTimer = undefined;
                }
                void queueApply();
            }),
            'interface_scale_listener_failed',
        ),
        registerListener(adapter.onMoved(scheduleApply), 'interface_scale_listener_failed'),
        registerListener(adapter.onResized(scheduleApply), 'interface_scale_listener_failed'),
    ]);
    await queueApply();

    return {
        state: () => ({ ...current }),
        setMode: async (mode) => {
            if (disposed) return;
            current = { ...current, mode };
            try {
                await persistMode(mode);
            } catch (error) {
                report('interface_scale_persistence_failed', { message: errorMessage(error) }, 'warn');
            }
            notify();
            await queueApply();
        },
        subscribe: (listener) => {
            listeners.add(listener);
            listener({ ...current });
            return () => listeners.delete(listener);
        },
        dispose: async () => {
            if (disposed) return;
            disposed = true;
            if (windowEventTimer !== undefined) clearTimeout(windowEventTimer);
            windowEventTimer = undefined;
            for (const unlisten of unlisteners.splice(0)) unlisten();
            await updateQueue;
            listeners.clear();
        },
    };
}
