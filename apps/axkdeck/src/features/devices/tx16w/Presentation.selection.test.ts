import { fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import Tx16wPresentationHarness from '../../../test/Tx16wPresentationHarness.svelte';
import { nativeDocumentFixture, nativeObjects, tx16wDocument } from '../../../test/nativeDocumentFixture';
import { ViewMemory } from '../../workspace/viewMemory';

const collection = () => within(screen.getByRole('region', { name: 'TX16W native objects' }));
const voice = (index: number) =>
    collection().getByRole('button', { name: new RegExp(`^Voice ${String(index).padStart(2, '0')}`) });
const selected = () =>
    collection()
        .queryAllByRole('button', { pressed: true })
        .map((button) => button.querySelector('strong')?.textContent?.trim());

async function setup() {
    const transport = nativeDocumentFixture();
    const originalDetail = transport.nativeObjectDetail;
    transport.nativeObjects = async () =>
        nativeObjects.map((item) => ({ ...item, type: 'voice', name: item.name.replace('Wave', 'Voice') }));
    transport.nativeObjectDetail = async (session, id) => {
        const detail = await originalDetail(session, id);
        if (detail.formatId !== 'yamaha.tx16w') throw new Error('Expected TX16W fixture detail');
        return {
            ...detail,
            name: detail.name.replace('Wave', 'Voice'),
            type: 'voice',
            details: { type: 'voice', slot: 1, regions: [] },
        };
    };
    render(Tx16wPresentationHarness, {
        transport,
        sessionId: 1,
        revision: 1,
        document: tx16wDocument,
        sources: [],
        remember: new ViewMemory().forDocument(1, tx16wDocument.id),
        onrevealFiles: vi.fn(),
    });
    await fireEvent.click(screen.getByRole('button', { name: 'Voices' }));
    await collection().findByRole('button', { name: /Voice 00/ });
}

describe('TX16W collection selection', () => {
    it.each(['metadata', 'padding'] as const)('selects an object from its %s area', async (target) => {
        await setup();
        const button = voice(20);
        // The old table cell/padding and the shared whole-row control represent the same click area.
        const row = button.closest('tr') ?? button;
        const metadata = row.querySelector('td:last-child, small, .object-size') ?? row.lastElementChild;
        expect(metadata).not.toBeNull();
        await fireEvent.click(target === 'padding' ? row : metadata!);
        expect(selected()).toEqual(['Voice 20']);
        expect(await screen.findByRole('heading', { name: 'Voice 20' })).toBeTruthy();
    });

    it.each(['ctrlKey', 'metaKey'] as const)(
        'toggles disjoint items with %s without replacing the active inspector',
        async (modifier) => {
            await setup();
            await fireEvent.click(voice(2));
            await screen.findByRole('heading', { name: 'Voice 02' });
            await fireEvent.click(voice(5), { [modifier]: true });
            expect(selected()).toEqual(['Voice 02', 'Voice 05']);
            expect(screen.getByRole('heading', { name: 'Voice 02' })).toBeTruthy();
            await fireEvent.click(voice(2), { [modifier]: true });
            expect(selected()).toEqual(['Voice 05']);
            await fireEvent.click(voice(8));
            expect(selected()).toEqual(['Voice 08']);
            expect(await screen.findByRole('heading', { name: 'Voice 08' })).toBeTruthy();
        },
    );

    it('extends and contracts a range from its anchor with Shift', async () => {
        await setup();
        await fireEvent.click(voice(2));
        await fireEvent.click(voice(5), { shiftKey: true });
        expect(selected()).toEqual(['Voice 02', 'Voice 03', 'Voice 04', 'Voice 05']);
        await fireEvent.click(voice(3), { shiftKey: true });
        expect(selected()).toEqual(['Voice 02', 'Voice 03']);
    });

    it('adds a range without discarding disjoint selection with Ctrl+Shift', async () => {
        await setup();
        await fireEvent.click(voice(2));
        await fireEvent.click(voice(5), { ctrlKey: true });
        await fireEvent.click(voice(7), { ctrlKey: true, shiftKey: true });
        expect(selected()).toEqual(['Voice 02', 'Voice 05', 'Voice 06', 'Voice 07']);
    });

    it('extends keyboard selection with Shift and replaces it with unmodified navigation', async () => {
        await setup();
        await fireEvent.click(voice(2));
        await fireEvent.keyDown(voice(2), { key: 'ArrowDown', shiftKey: true });
        expect(selected()).toEqual(['Voice 02', 'Voice 03']);
        await waitFor(() => expect(document.activeElement).toBe(voice(3)));
        await fireEvent.keyDown(voice(3), { key: 'ArrowDown', shiftKey: true });
        expect(selected()).toEqual(['Voice 02', 'Voice 03', 'Voice 04']);
        await fireEvent.keyDown(voice(4), { key: 'ArrowDown' });
        expect(selected()).toEqual(['Voice 05']);
        await waitFor(() => expect(document.activeElement).toBe(voice(5)));
    });

    it('ranges over visible results rather than hidden rows', async () => {
        await setup();
        await fireEvent.input(screen.getByRole('searchbox', { name: 'Search Voices' }), {
            target: { value: 'Voice 1' },
        });
        await fireEvent.click(voice(10));
        await fireEvent.click(voice(12), { shiftKey: true });
        expect(selected()).toEqual(['Voice 10', 'Voice 11', 'Voice 12']);
        await fireEvent.input(screen.getByRole('searchbox', { name: 'Search Voices' }), { target: { value: '' } });
        expect(selected()).toEqual(['Voice 10', 'Voice 11', 'Voice 12']);
    });

    it.each(['ctrlKey', 'metaKey'] as const)('selects all visible items with %s+A', async (modifier) => {
        await setup();
        await fireEvent.input(screen.getByRole('searchbox', { name: 'Search Voices' }), {
            target: { value: 'Voice 1' },
        });
        await fireEvent.click(voice(10));
        await fireEvent.keyDown(voice(10), { key: 'a', [modifier]: true });
        expect(selected()).toEqual(Array.from({ length: 10 }, (_, index) => `Voice ${index + 10}`));
    });
});
