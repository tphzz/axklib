import { afterEach, describe, expect, it, vi } from 'vitest';

import { saveClientDownload } from './clientFiles';

describe('saveClientDownload', () => {
    afterEach(() => vi.unstubAllGlobals());

    it('uses a temporary object URL and revokes it after starting the browser download', () => {
        const click = vi.fn();
        const remove = vi.fn();
        const append = vi.spyOn(document.body, 'append').mockImplementation(() => undefined);
        const createElement = vi.spyOn(document, 'createElement').mockReturnValue({
            click,
            remove,
            href: '',
            download: '',
        } as unknown as HTMLAnchorElement);
        const createObjectURL = vi.fn(() => 'blob:download-one');
        const revokeObjectURL = vi.fn();
        vi.stubGlobal('URL', { createObjectURL, revokeObjectURL });

        const blob = new Blob(['payload']);
        saveClientDownload({ filename: 'export.tar', blob });

        expect(createObjectURL).toHaveBeenCalledWith(blob);
        expect(click).toHaveBeenCalledOnce();
        expect(remove).toHaveBeenCalledOnce();
        expect(revokeObjectURL).toHaveBeenCalledWith('blob:download-one');
        append.mockRestore();
        createElement.mockRestore();
    });
});
