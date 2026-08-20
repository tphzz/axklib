import { describe, expect, it } from 'vitest';

import { shouldUseDirectComputerFileOperations } from './fileOperationRouting';

describe('shouldUseDirectComputerFileOperations', () => {
    it('uses native file dialogs for the managed local desktop sidecar', () => {
        expect(shouldUseDirectComputerFileOperations(true, 'local')).toBe(true);
    });

    it('retains the storage chooser for configured remote connections', () => {
        expect(shouldUseDirectComputerFileOperations(true, 'remote')).toBe(false);
    });

    it('does not infer locality from a configured loopback URL', () => {
        expect(shouldUseDirectComputerFileOperations(true, 'remote')).toBe(false);
    });

    it('does not use native file dialogs outside the desktop shell', () => {
        expect(shouldUseDirectComputerFileOperations(false, 'local')).toBe(false);
    });
});
