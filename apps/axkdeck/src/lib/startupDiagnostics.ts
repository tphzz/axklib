import { invoke } from '@tauri-apps/api/core';

export type StartupView = 'workspace' | 'allocation';

export interface FrontendStartupMetrics {
    schemaVersion: 2;
    view: StartupView;
    moduleEvaluatedMs: number;
    diagnosticsInstalledMs: number;
    interfaceScaleCompleteMs: number;
    shellMountedMs: number;
    shellFirstFrameMs: number;
    appModuleReadyMs: number;
    serverConnectionCompleteMs: number | null;
    appMountedMs: number;
    appFirstFrameMs: number;
    navigationDurationMs: number | null;
    firstContentfulPaintMs: number | null;
}

interface StartupPerformance {
    now(): number;
    getEntriesByType(type: string): PerformanceEntry[];
}

type StartupSubmitter = (metrics: FrontendStartupMetrics) => Promise<unknown>;
type FrameRequester = (callback: FrameRequestCallback) => number;

function finiteTiming(value: unknown): number | null {
    return typeof value === 'number' && Number.isFinite(value) && value >= 0 ? value : null;
}

function entryTiming(entries: PerformanceEntry[], name: string, property: 'duration' | 'startTime'): number | null {
    const entry = entries.find((candidate) => candidate.name === name) ?? entries[0];
    return finiteTiming(entry?.[property]);
}

function afterTwoFrames(requestFrame: FrameRequester): Promise<void> {
    return new Promise<void>((resolve) => requestFrame(() => requestFrame(() => resolve())));
}

export class FrontendStartupRecorder {
    private readonly moduleEvaluatedMs: number;
    private diagnosticsInstalledMs: number | null = null;
    private interfaceScaleCompleteMs: number | null = null;
    private shellMountedMs: number | null = null;
    private shellFirstFrameMs: number | null = null;
    private appModuleReadyMs: number | null = null;
    private serverConnectionCompleteMs: number | null = null;
    private appMountedMs: number | null = null;
    private shellPaint: Promise<void> | null = null;
    private reporting: Promise<void> | null = null;

    constructor(private readonly performanceSource: StartupPerformance) {
        this.moduleEvaluatedMs = this.performanceSource.now();
    }

    markDiagnosticsInstalled(): void {
        this.diagnosticsInstalledMs ??= this.performanceSource.now();
    }

    markInterfaceScaleComplete(): void {
        this.interfaceScaleCompleteMs ??= this.performanceSource.now();
    }

    markShellMounted(): void {
        this.shellMountedMs ??= this.performanceSource.now();
    }

    waitForShellFirstFrame(requestFrame: FrameRequester = window.requestAnimationFrame.bind(window)): Promise<void> {
        if (!this.shellPaint) {
            this.shellPaint = afterTwoFrames(requestFrame).then(() => {
                this.shellFirstFrameMs ??= this.performanceSource.now();
            });
        }
        return this.shellPaint;
    }

    markAppModuleReady(): void {
        this.appModuleReadyMs ??= this.performanceSource.now();
    }

    markServerConnectionComplete(): void {
        this.serverConnectionCompleteMs ??= this.performanceSource.now();
    }

    markAppMounted(): void {
        this.appMountedMs ??= this.performanceSource.now();
    }

    reportAfterAppFirstFrame(
        view: StartupView,
        submit: StartupSubmitter = (metrics) => invoke('complete_startup', { metrics }),
        requestFrame: FrameRequester = window.requestAnimationFrame.bind(window),
    ): Promise<void> {
        if (!this.reporting) this.reporting = this.report(view, submit, requestFrame);
        return this.reporting;
    }

    private async report(view: StartupView, submit: StartupSubmitter, requestFrame: FrameRequester): Promise<void> {
        await afterTwoFrames(requestFrame);
        const appFirstFrameMs = this.performanceSource.now();
        const fallback = appFirstFrameMs;
        await submit({
            schemaVersion: 2,
            view,
            moduleEvaluatedMs: this.moduleEvaluatedMs,
            diagnosticsInstalledMs: this.diagnosticsInstalledMs ?? fallback,
            interfaceScaleCompleteMs: this.interfaceScaleCompleteMs ?? fallback,
            shellMountedMs: this.shellMountedMs ?? fallback,
            shellFirstFrameMs: this.shellFirstFrameMs ?? fallback,
            appModuleReadyMs: this.appModuleReadyMs ?? fallback,
            serverConnectionCompleteMs: this.serverConnectionCompleteMs,
            appMountedMs: this.appMountedMs ?? fallback,
            appFirstFrameMs,
            navigationDurationMs: entryTiming(
                this.performanceSource.getEntriesByType('navigation'),
                'navigation',
                'duration',
            ),
            firstContentfulPaintMs: entryTiming(
                this.performanceSource.getEntriesByType('paint'),
                'first-contentful-paint',
                'startTime',
            ),
        });
    }
}

export const frontendStartup = new FrontendStartupRecorder(window.performance);
