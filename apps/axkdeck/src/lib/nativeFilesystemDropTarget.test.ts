import { expect, it, vi } from 'vitest';
import { PhysicalPosition } from '@tauri-apps/api/dpi';
import { dispatchNativeFilesystemDrop, registerNativeFilesystemDropTarget } from './nativeFilesystemDropTarget';

it('converts native coordinates once and unregisters only its own mounted receiver', () => {
    const first = vi.fn(),
        second = vi.fn();
    const unregisterFirst = registerNativeFilesystemDropTarget(first);
    const unregisterSecond = registerNativeFilesystemDropTarget(second);
    unregisterFirst();
    dispatchNativeFilesystemDrop({ type: 'drop', paths: ['/RAW'], position: new PhysicalPosition(300, 150) }, 1.5);
    expect(first).not.toHaveBeenCalled();
    expect(second).toHaveBeenCalledWith(expect.objectContaining({ type: 'drop', paths: ['/RAW'] }), { x: 200, y: 100 });
    dispatchNativeFilesystemDrop({ type: 'leave' }, 1.5);
    expect(second).toHaveBeenLastCalledWith({ type: 'leave' }, null);
    unregisterSecond();
    dispatchNativeFilesystemDrop({ type: 'leave' }, 1);
    expect(second).toHaveBeenCalledTimes(2);
});
