import { fireEvent, render, screen, within } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import Tx16wPresentationHarness from '../../../test/Tx16wPresentationHarness.svelte';
import { nativeDocumentFixture, nativeObjects, tx16wDocument } from '../../../test/nativeDocumentFixture';
import { ViewMemory } from '../../workspace/viewMemory';

describe('TX16W compact collection identities', () => {
    it.each([
        ['performance', 'Performances'],
        ['voice', 'Voices'],
        ['timbre', 'Timbres'],
        ['setup', 'Setups'],
    ])('keeps the %s name and size together inside the row identity', async (type, tab) => {
        const transport = nativeDocumentFixture();
        transport.nativeObjects = async () => [
            { ...nativeObjects[0]!, id: 'native-object', type, name: '12string', sizeBytes: 146 },
        ];
        render(Tx16wPresentationHarness, {
            transport,
            sessionId: 1,
            revision: 1,
            document: tx16wDocument,
            sources: [],
            remember: new ViewMemory().forDocument(1, tx16wDocument.id),
            onrevealFiles: vi.fn(),
        });
        await fireEvent.click(screen.getByRole('button', { name: tab }));
        const collection = within(screen.getByRole('region', { name: 'TX16W native objects' }));
        const row = await collection.findByRole('button', { name: /12string.*146 B/ });
        const name = within(row).getByText('12string');
        const size = within(row).getByText('146 B');
        expect(name.parentElement?.parentElement).toBe(size.parentElement);
        expect(size.parentElement).not.toBe(row);
        expect(name.getAttribute('title')).toBe('12string');
        expect(size.getAttribute('title')).toBe('146 B');
        expect(row.querySelector('.collection-row-slot')).toBeNull();
    });
});
