import { beforeEach, describe, expect, it, vi } from 'vitest';

const mocks = vi.hoisted(() => ({ invoke: vi.fn() }));

vi.mock('@tauri-apps/api/core', () => ({ invoke: mocks.invoke }));

import { selectLocalPackage, selectLocalPackages } from './nativePackages';

describe('nativePackages', () => {
    beforeEach(() => mocks.invoke.mockReset());

    it('passes the preferred package path to the native chooser', async () => {
        mocks.invoke.mockResolvedValue(null);

        await selectLocalPackage('previous-package.axkvol');

        expect(mocks.invoke).toHaveBeenCalledWith('select_local_package', {
            preferredPath: 'previous-package.axkvol',
        });
    });

    it('uses an explicit null preference for the first native package selection', async () => {
        mocks.invoke.mockResolvedValue(null);

        await selectLocalPackage(null);

        expect(mocks.invoke).toHaveBeenCalledWith('select_local_package', { preferredPath: null });
    });

    it('passes the preferred package path to the multi-file native chooser', async () => {
        mocks.invoke.mockResolvedValue([]);

        await selectLocalPackages('previous-package.axkprg');

        expect(mocks.invoke).toHaveBeenCalledWith('select_local_packages', {
            preferredPath: 'previous-package.axkprg',
        });
    });
});
