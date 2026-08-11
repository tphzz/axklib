import { describe, expect, it, vi } from 'vitest';
import { PickerController } from './picker';

describe('PickerController', () => {
    it('settles a replaced request as cancelled before publishing the replacement', async () => {
        const changes = vi.fn();
        const picker = new PickerController(changes);
        const first = picker.chooseLocation('directory', 'First');
        const second = picker.chooseFiles('Second', ['wav']);

        await expect(first).resolves.toBeNull();
        expect(changes.mock.calls.map(([request]) => request?.title ?? null)).toEqual([null, 'First', null, 'Second']);

        picker.finish([]);
        await expect(second).resolves.toEqual([]);
    });

    it('cancels the pending request during disposal', async () => {
        const picker = new PickerController(() => undefined);
        const pending = picker.chooseLocation('media-source', 'Open image');
        picker.dispose();
        await expect(pending).resolves.toBeNull();
    });
});
