import { render, screen } from '@testing-library/svelte';
import { describe, expect, it } from 'vitest';
import CollectionIdentity from './CollectionIdentity.svelte';

describe('compact collection identity', () => {
    it('exposes the complete name and metadata for truncated labels', () => {
        render(CollectionIdentity, { name: 'A long sampler object name', metadata: '146 B' });
        expect(screen.getByText('A long sampler object name').title).toBe('A long sampler object name');
        expect(screen.getByText('146 B').title).toBe('146 B');
    });

    it('updates format-provided tooltips and removes absent metadata', async () => {
        const view = render(CollectionIdentity, {
            name: 'Program',
            metadata: '128 B',
            nameTitle: 'Native Program',
            metadataTitle: 'Stored record size: 128 B',
        });
        expect(screen.getByText('Program').title).toBe('Native Program');
        expect(screen.getByText('128 B').title).toBe('Stored record size: 128 B');
        await view.rerender({ name: 'Setup', metadata: '', nameTitle: undefined, metadataTitle: undefined });
        expect(screen.getByText('Setup').title).toBe('Setup');
        expect(view.container.querySelector('small')).toBeNull();
    });
});
