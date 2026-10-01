import type { FilesystemEntry } from '../../lib/filesystem';
import type { FilesController } from './controller.svelte';
import { moveSelection } from './moveSelection';

interface MoveGestureHost {
    controller(): FilesController;
    scroller(): HTMLElement;
    blocked(): boolean;
    canExport(): boolean;
    canMove(entries: FilesystemEntry[], target: FilesystemEntry | null): boolean;
    move(entries: FilesystemEntry[], target: FilesystemEntry): void;
    export(entries: FilesystemEntry[], revision: number, rootId: string): void;
    show(target: FilesystemEntry | null, active: boolean, count: number): void;
}

export class FilesMoveGesture {
    private armed: {
        x: number;
        y: number;
        id: number;
        entry: FilesystemEntry;
        controller: FilesController;
        revision: number;
        rootId: string;
    } | null = null;
    private entries: FilesystemEntry[] = [];
    private target: FilesystemEntry | null = null;
    private timer: ReturnType<typeof setInterval> | undefined;
    private hover = 0;
    private x = 0;
    private y = 0;
    suppressClick = false;
    get active(): boolean {
        return this.entries.length > 0;
    }
    constructor(private host: MoveGestureHost) {}

    arm(event: PointerEvent, entry: FilesystemEntry): void {
        this.suppressClick = false;
        if (
            event.button !== 0 ||
            event.ctrlKey ||
            event.metaKey ||
            event.shiftKey ||
            this.host.blocked() ||
            (!this.host.canExport() && !this.host.controller().capabilities?.moveEntry) ||
            (event.target instanceof Element && event.target.closest('button,input'))
        )
            return;
        const controller = this.host.controller();
        this.armed = {
            x: event.clientX,
            y: event.clientY,
            id: event.pointerId,
            entry,
            controller,
            revision: controller.revision,
            rootId: controller.rootId,
        };
    }

    update(event: PointerEvent): void {
        const armed = this.armed;
        if (!armed || armed.id !== event.pointerId) return;
        if (!(event.buttons & 1) || !this.current()) {
            this.cancel();
            return;
        }
        if (!this.active && Math.hypot(event.clientX - armed.x, event.clientY - armed.y) < 8) return;
        event.preventDefault();
        if (!this.active) {
            armed.controller.selectForContext(armed.entry);
            this.entries = [...armed.controller.selection];
            this.suppressClick = true;
            this.host.scroller().setPointerCapture?.(armed.id);
            this.timer = setInterval(() => this.track(), 30);
        }
        this.x = event.clientX;
        this.y = event.clientY;
        if (this.x < 0 || this.y < 0 || this.x >= window.innerWidth || this.y >= window.innerHeight) {
            const entries = this.entries;
            this.cancel();
            if (this.host.canExport()) this.host.export(entries, armed.revision, armed.rootId);
            return;
        }
        this.track();
    }

    private current(): boolean {
        const armed = this.armed;
        return (
            !!armed &&
            this.host.controller() === armed.controller &&
            armed.controller.revision === armed.revision &&
            armed.controller.rootId === armed.rootId
        );
    }

    private track(): void {
        if (!this.active || !this.current()) {
            this.cancel();
            return;
        }
        const scroller = this.host.scroller();
        const element = document.elementFromPoint?.(this.x, this.y);
        let target: FilesystemEntry | null = null;
        if (element && !element.closest('[role="dialog"],[role="menu"]')) {
            const root = element.closest('[data-files-root-drop]');
            if (root && scroller.closest('.files-workspace')?.contains(root)) target = this.armed!.controller.root;
            else if (scroller.contains(element)) {
                const id = element.closest<HTMLElement>('[data-file-entry]')?.dataset.fileEntry;
                target = id
                    ? (this.armed!.controller.rows.find((row) => row.entry.id === id)?.entry ?? null)
                    : this.armed!.controller.root;
                const rect = scroller.getBoundingClientRect();
                if (this.y < rect.top + 32) scroller.scrollTop -= 12;
                else if (this.y > rect.bottom - 32) scroller.scrollTop += 12;
            }
        }
        if (!this.host.canMove(this.entries, target)) target = null;
        if (this.target?.id !== target?.id) this.hover = Date.now();
        this.target = target;
        const controller = this.armed!.controller;
        if (target?.kind === 'directory' && Date.now() - this.hover >= 650 && !controller.expanded(target.id)) {
            this.hover = Number.POSITIVE_INFINITY;
            void controller.toggle(target);
        }
        this.host.show(target, true, target ? moveSelection(this.entries, target).length : this.entries.length);
    }

    release(event: PointerEvent): void {
        if (this.armed?.id !== event.pointerId) return;
        this.x = event.clientX;
        this.y = event.clientY;
        if (this.active) this.track();
        const entries = this.entries;
        const target = this.target;
        const current = this.current();
        this.cancel();
        if (current && target && this.host.canMove(entries, target)) this.host.move(entries, target);
    }

    cancel(): void {
        clearInterval(this.timer);
        const id = this.armed?.id;
        const scroller = this.host.scroller();
        if (id !== undefined && scroller?.hasPointerCapture?.(id)) scroller.releasePointerCapture(id);
        this.armed = null;
        this.entries = [];
        this.target = null;
        this.host.show(null, false, 0);
    }
}
