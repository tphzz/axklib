import { fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import Tx16wPresentationHarness from '../../../test/Tx16wPresentationHarness.svelte';
import { nativeDocumentFixture, nativeObjects, tx16wDocument } from '../../../test/nativeDocumentFixture';
import { ViewMemory } from '../../workspace/viewMemory';
import type { ImageTransport } from '../../../lib/transport';

const labels = ['Performances', 'Voices', 'Timbres', 'Waves', 'Setups', 'Native Files'];
const collection = () => within(screen.getByRole('region', { name: 'TX16W native objects' }));
const inspector = () => within(screen.getByRole('complementary', { name: 'TX16W inspector' }));
function setup(transport = nativeDocumentFixture(), memory = new ViewMemory(), documentId = tx16wDocument.id) {
    const onconvert = vi.fn();
    return {
        onconvert,
        ...render(Tx16wPresentationHarness, {
            transport,
            sessionId: 1,
            revision: 1,
            document: { ...tx16wDocument, id: documentId },
            sources: [],
            remember: memory.forDocument(1, documentId),
            onrevealFiles: vi.fn(),
            onconvert,
        }),
    };
}

describe('TX16W workspace conventions', () => {
    it('has one structural root, ordered collection tabs, no all-objects view, and an empty default collection', async () => {
        setup();
        const navigation = screen.getByRole('navigation', { name: 'TX16W structure' });
        expect(
            within(navigation)
                .getAllByRole('button')
                .map((node) => node.textContent?.trim()),
        ).toEqual(['/']);
        expect(
            within(screen.getByRole('navigation', { name: 'Workspace views' }))
                .getAllByRole('button')
                .map((node) => node.textContent?.trim()),
        ).toEqual(labels);
        expect(screen.getByRole('button', { name: 'Performances' }).getAttribute('aria-pressed')).toBe('true');
        expect(screen.queryByRole('button', { name: /All objects/i })).toBeNull();
        expect(screen.queryByRole('button', { name: 'Convert' })).toBeNull();
        await collection().findByText('No performances');
        await fireEvent.click(screen.getByRole('button', { name: 'Waves' }));
        await collection().findByRole('button', { name: 'Wave 20' });
        expect(
            collection()
                .getAllByRole('columnheader')
                .map((node) => node.textContent),
        ).toEqual(['Name', 'Bytes']);
        expect(screen.getByRole('searchbox', { name: 'Search Waves' })).toBeTruthy();
    });

    it('retains search and selection per tab and document, including companion remounts', async () => {
        const memory = new ViewMemory();
        const view = setup(nativeDocumentFixture(), memory);
        await fireEvent.click(screen.getByRole('button', { name: 'Waves' }));
        await fireEvent.click(await collection().findByRole('button', { name: 'Wave 20' }));
        await inspector().findByRole('heading', { name: 'Wave 20' });
        await fireEvent.input(screen.getByRole('searchbox', { name: 'Search Waves' }), { target: { value: 'Wave 2' } });
        await fireEvent.click(screen.getByRole('button', { name: 'Voices' }));
        expect(inspector().getByText('No object selected')).toBeTruthy();
        await fireEvent.input(screen.getByRole('searchbox', { name: 'Search Voices' }), { target: { value: 'Piano' } });
        await fireEvent.click(screen.getByRole('button', { name: 'Waves' }));
        expect(screen.getByRole('searchbox', { name: 'Search Waves' })).toHaveProperty('value', 'Wave 2');
        await inspector().findByRole('heading', { name: 'Wave 20' });
        view.unmount();
        const other = setup(nativeDocumentFixture(), memory, 'other-document');
        expect(screen.getByRole('button', { name: 'Performances' }).getAttribute('aria-pressed')).toBe('true');
        other.unmount();
        setup(nativeDocumentFixture(), memory);
        expect(screen.getByRole('searchbox', { name: 'Search Waves' })).toHaveProperty('value', 'Wave 2');
        await inspector().findByRole('heading', { name: 'Wave 20' });
        await fireEvent.click(screen.getByRole('button', { name: 'Voices' }));
        expect(screen.getByRole('searchbox', { name: 'Search Voices' })).toHaveProperty('value', 'Piano');
    });

    it('reveals relationships in their target tab and clears an obstructing search', async () => {
        const transport = nativeDocumentFixture();
        const originalDetail = transport.nativeObjectDetail;
        transport.nativeObjects = async () => [
            ...nativeObjects,
            { ...nativeObjects[0]!, id: 'voice-1', type: 'voice', name: 'Voice One' },
        ];
        transport.nativeRelationships = async () => [
            {
                id: 'relationship',
                sourceObjectId: 'voice-1',
                targetObjectId: 'wave-20',
                candidateObjectIds: [],
                type: 'native-reference',
                quality: 'RESOLVED',
                basis: 'Native reference',
                notes: [],
                assignmentIndex: null,
                assignmentName: '',
                assignmentState: '',
                receiveChannelDisplay: '',
            },
        ];
        transport.nativeObjectDetail = async (session, id) =>
            id === 'voice-1'
                ? {
                      ...(await originalDetail(session, 'wave-20')),
                      objectId: id,
                      name: 'Voice One',
                      type: 'voice',
                      details: { type: 'voice', slot: 1, regions: [] },
                  }
                : originalDetail(session, id);
        setup(transport);
        await fireEvent.click(screen.getByRole('button', { name: 'Waves' }));
        await fireEvent.input(screen.getByRole('searchbox', { name: 'Search Waves' }), {
            target: { value: 'not found' },
        });
        await fireEvent.click(screen.getByRole('button', { name: 'Voices' }));
        await fireEvent.click(await collection().findByRole('button', { name: 'Voice One' }));
        await fireEvent.click(await inspector().findByRole('button', { name: 'Wave 20' }));
        expect(screen.getByRole('searchbox', { name: 'Search Waves' })).toHaveProperty('value', '');
        await waitFor(() => expect(document.activeElement).toBe(collection().getByRole('button', { name: 'Wave 20' })));
        await inspector().findByRole('heading', { name: 'Wave 20' });
    });

    it('opens root conversion with pointer or keyboard and restores focus on Escape', async () => {
        const { onconvert } = setup();
        const root = screen.getByRole('button', { name: '/' });
        await fireEvent.contextMenu(root, { clientX: 20, clientY: 30 });
        await fireEvent.click(await screen.findByRole('menuitem', { name: 'Convert...' }));
        expect(onconvert).toHaveBeenCalledOnce();
        await fireEvent.keyDown(root, { key: 'F10', shiftKey: true });
        const item = await screen.findByRole('menuitem', { name: 'Convert...' });
        await fireEvent.keyDown(item, { key: 'Escape' });
        await waitFor(() => expect(screen.queryByRole('menu')).toBeNull());
        expect(document.activeElement).toBe(root);
    });

    it('does not show an old detail after switching collections', async () => {
        const transport = nativeDocumentFixture();
        let release!: (value: Awaited<ReturnType<ImageTransport['nativeObjectDetail']>>) => void;
        const detail = await transport.nativeObjectDetail(1, 'wave-20');
        transport.nativeObjectDetail = () =>
            new Promise((resolve) => {
                release = resolve;
            });
        setup(transport);
        await fireEvent.click(screen.getByRole('button', { name: 'Waves' }));
        await fireEvent.click(await collection().findByRole('button', { name: 'Wave 20' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Voices' }));
        release(detail);
        await waitFor(() => expect(inspector().getByText('No object selected')).toBeTruthy());
        expect(inspector().queryByRole('heading', { name: 'Wave 20' })).toBeNull();
    });

    it('pages by complete row heights and supports boundary navigation', async () => {
        setup();
        await fireEvent.click(screen.getByRole('button', { name: 'Waves' }));
        const button = await collection().findByRole('button', { name: 'Wave 20' });
        Object.defineProperty(button.closest('[data-navigation-list]'), 'clientHeight', { value: 300 });
        Object.defineProperty(button, 'offsetHeight', { value: 18 });
        Object.defineProperty(button.closest('tr'), 'offsetHeight', { value: 30 });
        await fireEvent.click(button);
        await fireEvent.keyDown(button, { key: 'PageDown' });
        await waitFor(() => expect(document.activeElement).toBe(collection().getByRole('button', { name: 'Wave 29' })));
        await fireEvent.keyDown(document.activeElement!, { key: 'Home' });
        await waitFor(() => expect(document.activeElement).toBe(collection().getByRole('button', { name: 'Wave 00' })));
        await fireEvent.keyDown(document.activeElement!, { key: 'End' });
        await waitFor(() => expect(document.activeElement).toBe(collection().getByRole('button', { name: 'Wave 59' })));
    });

    it('keeps unsupported records inspectable in Native Files without mixing them into collections', async () => {
        const transport = nativeDocumentFixture();
        const detail = await transport.nativeObjectDetail(1, 'wave-20');
        transport.nativeObjects = async () => [
            ...nativeObjects,
            { ...nativeObjects[0]!, id: 'unknown', name: 'UNSUPPORTED.C01', type: 'native-file' },
        ];
        transport.nativeObjectDetail = async () => ({
            ...detail,
            objectId: 'unknown',
            name: 'UNSUPPORTED.C01',
            type: 'native-file',
            details: { type: 'opaque', kind: 'unsupported' },
        });
        setup(transport);
        await collection().findByText('No performances');
        expect(collection().queryByRole('button', { name: 'UNSUPPORTED.C01' })).toBeNull();
        await fireEvent.click(screen.getByRole('button', { name: 'Native Files' }));
        await fireEvent.click(await collection().findByRole('button', { name: 'UNSUPPORTED.C01' }));
        await inspector().findByText('unsupported');
        expect(inspector().getByRole('button', { name: 'To Files' })).toBeTruthy();
    });

    it('waits for native inventory before revealing an object requested from Files', async () => {
        const transport = nativeDocumentFixture();
        let release!: () => void;
        transport.nativeObjects = async () => {
            await new Promise<void>((resolve) => {
                release = resolve;
            });
            return nativeObjects;
        };
        setup(transport);
        await fireEvent.click(screen.getByRole('button', { name: 'Reveal wave' }));
        release();
        await inspector().findByRole('heading', { name: 'Wave 20' });
        expect(screen.getByRole('button', { name: 'Waves' }).getAttribute('aria-pressed')).toBe('true');
        await waitFor(() => expect(document.activeElement).toBe(collection().getByRole('button', { name: 'Wave 20' })));
    });
});
