import { fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import Tx16wPresentationHarness from '../../../test/Tx16wPresentationHarness.svelte';
import { nativeDocumentFixture, nativeObjects, tx16wDocument } from '../../../test/nativeDocumentFixture';
import type { ImageTransport } from '../../../lib/transport';
import { ViewMemory } from '../../workspace/viewMemory';

const collection = () => within(screen.getByRole('region', { name: 'TX16W native objects' }));

function setup(transport: ImageTransport) {
    const onrevealFiles = vi.fn();
    render(Tx16wPresentationHarness, {
        transport,
        sessionId: 1,
        revision: 1,
        document: tx16wDocument,
        sources: [],
        remember: new ViewMemory().forDocument(1, tx16wDocument.id),
        onrevealFiles,
    });
    return onrevealFiles;
}

describe('TX16W object names and identities', () => {
    it('uses the adapter wave name in the collection and inspector while revealing by object identity', async () => {
        const transport = nativeDocumentFixture();
        const object = { ...nativeObjects[0]!, name: 'Tone.W01' };
        const detail = await transport.nativeObjectDetail(1, object.id);
        transport.nativeObjects = async () => [object];
        transport.nativeObjectDetail = async () => ({
            ...detail,
            name: object.name,
            locations: [{ memberId: 'source-2', rootId: 'fat12', entryId: 'nested/Tone.W01', offset: 0, length: 512 }],
        });
        const reveal = setup(transport);
        await fireEvent.click(screen.getByRole('button', { name: 'Waves' }));
        await fireEvent.click(await collection().findByRole('button', { name: 'Inspect Tone.W01' }));
        expect(await screen.findByRole('heading', { name: object.name })).toBeTruthy();
        expect(collection().getByText(object.name)).toBeTruthy();
        await fireEvent.click(screen.getByRole('button', { name: 'To Files' }));
        expect(reveal).toHaveBeenCalledExactlyOnceWith(object.id);
    });

    it('keeps all 32 same-named performances independently selectable and inspectable', async () => {
        const transport = nativeDocumentFixture();
        const originalDetail = transport.nativeObjectDetail;
        const objects = nativeObjects.slice(0, 32).map((item) => ({
            ...item,
            type: 'performance',
            name: 'init performance',
            sizeBytes: 146,
        }));
        transport.nativeObjects = async () => objects;
        transport.nativeObjectDetail = async (session, id) => {
            const detail = await originalDetail(session, id);
            return {
                ...detail,
                name: 'init performance',
                type: 'performance',
                details: { type: 'performance', slot: objects.findIndex((item) => item.id === id), voices: [] },
            };
        };
        const reveal = setup(transport);
        const rows = await collection().findAllByRole('button', { name: /^init performance/ });
        expect(rows).toHaveLength(32);
        for (const slot of [10, 12, 31]) {
            await fireEvent.click(rows[slot]!);
            await screen.findByRole('heading', { name: 'init performance' });
            await waitFor(() =>
                expect(screen.getByText('Slot').parentElement?.querySelector('dd')?.textContent).toBe(String(slot)),
            );
            expect(collection().getAllByRole('button', { pressed: true })).toEqual([rows[slot]]);
            await fireEvent.click(screen.getByRole('button', { name: 'To Files' }));
            expect(reveal).toHaveBeenLastCalledWith(objects[slot]!.id);
        }
    });
});
