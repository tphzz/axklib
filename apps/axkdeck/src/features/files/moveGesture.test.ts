import { afterEach, expect, it, vi } from 'vitest';
import { FilesMoveGesture } from './moveGesture';
import { FilesController } from './controller.svelte';
import { moveTargetAllowed } from './moveSelection';
import { filesystemEntry, writableFilesRoot } from '../../lib/testing/filesystem';

afterEach(() => {
    vi.useRealTimers();
    vi.restoreAllMocks();
    document.body.replaceChildren();
});

async function fixture() {
    const root = filesystemEntry({ id: 'root', kind: 'root', parentId: null, path: '' });
    const file = filesystemEntry({ id: 'file', kind: 'file', name: 'A.BIN', path: '/A.BIN' });
    const target = filesystemEntry({ id: 'target', name: 'Target', path: '/Target' });
    const controller = new FilesController({
        inspect: async (query = {}) => ({
            revision: 7,
            available: true,
            deviceView: null,
            filesystemName: 'FAT',
            items: query.parentId === 'root' ? [file, target] : query.parentId ? [] : [root],
            totalCount: query.parentId === 'root' ? 2 : query.parentId ? 0 : 1,
            rootCapabilities: [writableFilesRoot],
        }),
    });
    await controller.initialize();
    const scroller = document.createElement('div');
    const row = document.createElement('div');
    row.dataset.fileEntry = target.id;
    scroller.append(row);
    document.body.append(scroller);
    Object.defineProperty(document, 'elementFromPoint', { configurable: true, value: () => row });
    const hit = vi.spyOn(document, 'elementFromPoint');
    const host = {
        controller: () => controller,
        scroller: () => scroller,
        blocked: () => false,
        canExport: () => false,
        canMove: moveTargetAllowed,
        move: vi.fn(),
        export: vi.fn(),
        show: vi.fn(),
    };
    const gesture = new FilesMoveGesture(host);
    const event = (x: number, buttons = 1) =>
        Object.assign(
            new MouseEvent('pointermove', {
                clientX: x,
                clientY: 50,
                button: 0,
                buttons,
            }),
            { pointerId: 1 },
        ) as PointerEvent;
    return { gesture, host, controller, file, target, hit, event };
}

it('rechecks the release position instead of moving to the last hovered target', async () => {
    const f = await fixture();
    f.gesture.arm(f.event(10), f.file);
    f.gesture.update(f.event(30));
    expect(f.gesture.active).toBe(true);
    f.hit.mockReturnValue(null);
    f.gesture.release(f.event(50, 0));
    expect(f.host.move).not.toHaveBeenCalled();
    expect(f.gesture.active).toBe(false);
});

it('cancels stale gestures and does not submit canceled gestures', async () => {
    const f = await fixture();
    f.gesture.arm(f.event(10), f.file);
    f.gesture.update(f.event(30));
    f.controller.revision++;
    f.gesture.release(f.event(30, 0));
    expect(f.host.move).not.toHaveBeenCalled();
    f.gesture.arm(f.event(10), f.file);
    f.gesture.update(f.event(30));
    f.gesture.cancel();
    f.gesture.release(f.event(30, 0));
    expect(f.host.move).not.toHaveBeenCalled();
});

it('expands a valid folder after hovering but waits for release before review', async () => {
    const f = await fixture();
    vi.useFakeTimers();
    const toggle = vi.spyOn(f.controller, 'toggle');
    f.gesture.arm(f.event(10), f.file);
    f.gesture.update(f.event(30));
    await vi.advanceTimersByTimeAsync(700);
    expect(toggle).toHaveBeenCalledOnce();
    expect(f.host.move).not.toHaveBeenCalled();
    f.gesture.release(f.event(30, 0));
    expect(f.host.move).toHaveBeenCalledWith([f.file], f.target);
    expect(f.host.export).not.toHaveBeenCalled();
});
