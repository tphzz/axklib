import { describe, expect, it, vi } from 'vitest';

import { InMemoryImageTransport } from './inMemoryTransport';
import { serverFileLocation } from '../storageLocations';

describe('InMemoryImageTransport', () => {
    it('provides one deterministic transport contract for UI conformance tests', async () => {
        const onClose = vi.fn();
        const transport = new InMemoryImageTransport({
            opened: {
                initialVolume: null,
                tree: [],
                validation: {
                    valid: true,
                    issueCount: 0,
                    errorCount: 0,
                    warningCount: 0,
                    objectCount: 0,
                    relationshipCount: 0,
                },
                objects: [],
                objectTotalCount: 0,
            },
            onClose,
        });

        const opened = await transport.openImage(
            serverFileLocation({
                rootId: 'workspace',
                relativePath: 'images/empty.hds',
            }),
        );
        await transport.closeImage(opened.sessionId);

        expect(opened.sessionId).toBe(1);
        expect(onClose).toHaveBeenCalledWith(1);
        expect(transport.calls).toEqual(['openImage', 'closeImage']);
        await expect(transport.startPackageImport('missing')).rejects.toThrow('startPackageImport is not configured');
    });
});
