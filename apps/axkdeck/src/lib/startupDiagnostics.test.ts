import { describe, expect, it, vi } from 'vitest';

import { FrontendStartupRecorder } from './startupDiagnostics';

describe('frontend startup diagnostics', () => {
    it('distinguishes the shell paint from application readiness', async () => {
        let now = 4;
        const recorder = new FrontendStartupRecorder({
            now: () => now,
            getEntriesByType: () => [],
        });
        const frames: FrameRequestCallback[] = [];
        const requestFrame = vi.fn((callback: FrameRequestCallback) => {
            frames.push(callback);
            return frames.length;
        });
        const submit = vi.fn().mockResolvedValue(undefined);

        now = 9;
        recorder.markShellMounted();
        now = 12;
        recorder.markDiagnosticsInstalled();
        now = 14;
        recorder.markInterfaceScaleComplete();

        const shellPaint = recorder.waitForShellFirstFrame(requestFrame);
        now = 20;
        frames.shift()?.(now);
        await Promise.resolve();
        now = 24;
        frames.shift()?.(now);
        await shellPaint;

        now = 29;
        recorder.markServerConnectionComplete();
        now = 34;
        recorder.markAppModuleReady();
        now = 38;
        recorder.markAppMounted();

        const reporting = recorder.reportAfterAppFirstFrame('workspace', submit, requestFrame);
        now = 42;
        frames.shift()?.(now);
        await Promise.resolve();
        now = 47;
        frames.shift()?.(now);
        await reporting;

        expect(submit).toHaveBeenCalledWith({
            schemaVersion: 2,
            view: 'workspace',
            moduleEvaluatedMs: 4,
            diagnosticsInstalledMs: 12,
            interfaceScaleCompleteMs: 14,
            shellMountedMs: 9,
            shellFirstFrameMs: 24,
            appModuleReadyMs: 34,
            serverConnectionCompleteMs: 29,
            appMountedMs: 38,
            appFirstFrameMs: 47,
            navigationDurationMs: null,
            firstContentfulPaintMs: null,
        });
    });

    it('allows views that do not require a server connection', async () => {
        const recorder = new FrontendStartupRecorder({
            now: () => 20,
            getEntriesByType: (type) => {
                if (type === 'navigation') return [{ duration: 18 }] as PerformanceEntry[];
                if (type === 'paint') {
                    return [{ name: 'first-contentful-paint', startTime: 12 }] as PerformanceEntry[];
                }
                return [];
            },
        });
        recorder.markShellMounted();
        recorder.markDiagnosticsInstalled();
        recorder.markInterfaceScaleComplete();
        const requestFrame = (callback: FrameRequestCallback) => {
            callback(20);
            return 1;
        };
        await recorder.waitForShellFirstFrame(requestFrame);
        recorder.markAppModuleReady();
        recorder.markAppMounted();
        const submit = vi.fn().mockResolvedValue(undefined);

        await recorder.reportAfterAppFirstFrame('allocation', submit, requestFrame);

        expect(submit.mock.calls[0]?.[0]).toMatchObject({
            view: 'allocation',
            serverConnectionCompleteMs: null,
            navigationDurationMs: 18,
            firstContentfulPaintMs: 12,
        });
    });
});
