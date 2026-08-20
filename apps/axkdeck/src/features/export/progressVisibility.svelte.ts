export const EXPORT_PROGRESS_DELAY_MS = 750;

export type ExportProgressOperation =
    | 'package-export'
    | 'audio-export'
    | 'sequence-export'
    | 'volume-package-export'
    | 'volume-floppy-export'
    | 'media-export';

export class DelayedExportProgressVisibility {
    operation = $state<ExportProgressOperation | null>(null);
    private pendingOperation: ExportProgressOperation | null = null;
    private timer: ReturnType<typeof setTimeout> | null = null;

    update(operation: ExportProgressOperation | null): void {
        if (operation === this.pendingOperation) return;
        this.clearTimer();
        this.pendingOperation = operation;
        this.operation = null;
        if (operation === null) return;
        this.timer = setTimeout(() => {
            if (this.pendingOperation === operation) this.operation = operation;
            this.timer = null;
        }, EXPORT_PROGRESS_DELAY_MS);
    }

    dispose(): void {
        this.clearTimer();
        this.pendingOperation = null;
        this.operation = null;
    }

    private clearTimer(): void {
        if (this.timer === null) return;
        clearTimeout(this.timer);
        this.timer = null;
    }
}
