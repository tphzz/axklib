import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it } from 'vitest';
import App from '../../App.svelte';
import AlternateWorkspace from '../../test/AlternateWorkspace.svelte';

describe('Workspace contributions', () => {
    it('accepts an independent backend and replaces all four zones without an ImageTransport', async () => {
        render(App, { backend: { id: 'alternate', label: 'Another backend', application: AlternateWorkspace } });
        expect(screen.getByRole('button', { name: 'Open image' })).toBeTruthy();
        expect(screen.getByText('device navigation')).toBeTruthy();
        expect(screen.getByText('device content')).toBeTruthy();
        expect(screen.getByText('Alternate device inspector')).toBeTruthy();
        await fireEvent.click(screen.getByRole('button', { name: 'Editor panel' }));
        expect(screen.getByText('Alternate device tools')).toBeTruthy();
        await fireEvent.click(screen.getByRole('button', { name: 'Files' }));
        expect(screen.getByText('files navigation')).toBeTruthy();
        expect(screen.getByText('files content')).toBeTruthy();
        expect(screen.getByText('Alternate files inspector')).toBeTruthy();
        expect(screen.queryByText('Alternate device tools')).toBeNull();
        expect(screen.getByRole('button', { name: 'Editor panel' })).toHaveProperty('disabled', true);
        await fireEvent.click(screen.getByRole('button', { name: 'Device' }));
        expect(screen.getByText('Alternate device tools')).toBeTruthy();
    });
});
