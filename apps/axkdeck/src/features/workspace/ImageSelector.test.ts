import { selectValue } from '../../test/selectControl';
import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import ImageSelector from './ImageSelector.svelte';
import { sourceLabels } from './sourceLabels';
import { nativeWorkspaceFixture } from '../../test/nativeWorkspaceFixture';

const sources = [
    nativeWorkspaceFixture().sources[0],
    {
        ...nativeWorkspaceFixture().sources[0],
        id: 'source-2',
        source: { kind: 'FILE' as const, file: { rootId: 'root', relativePath: 'second.hds' } },
    },
];

describe('Image selector', () => {
    it('lists physical filenames and restores a declined switch without a Files-only choice', async () => {
        const onselect = vi.fn(async () => undefined);
        render(ImageSelector, { sources, selected: 'source-1', disabled: false, onselect, onerror: vi.fn() });
        const control = screen.getByRole('combobox', { name: 'Image' }) as HTMLButtonElement;
        await fireEvent.click(control);
        expect(screen.getAllByRole('option').map((item) => item.textContent?.trim())).toEqual([
            'test.hds',
            'second.hds',
        ]);
        await selectValue(control, 'source-2');
        await waitFor(() => expect(control.value).toBe('source-1'));
        expect(onselect).toHaveBeenCalledWith('source-2');
        expect(control.title).toBe('root/test.hds');
    });

    it('disambiguates matching filenames by their full storage location', () => {
        const labels = sourceLabels([
            sources[0],
            { ...sources[1], source: { kind: 'FILE', file: { rootId: 'other', relativePath: 'test.hds' } } },
        ]);
        expect(labels.map((item) => item.label)).toEqual(['root/test.hds', 'other/test.hds']);
    });
});
