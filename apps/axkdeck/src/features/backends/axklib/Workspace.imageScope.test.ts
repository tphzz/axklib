import { selectValue } from '../../../test/selectControl';
import { fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { describe, expect, it } from 'vitest';
import ImageScopeHarness from '../../../test/ImageScopeHarness.svelte';
import { imageScopeFixture } from '../../../test/imageScopeFixture.svelte';

const image = () => screen.getByRole('combobox', { name: 'Image' }) as HTMLButtonElement;
const search = () => screen.getByRole('searchbox', { name: 'Search filesystem' }) as HTMLInputElement;
async function selectImage(value: string) {
    await selectValue(image(), value);
    await waitFor(() => {
        expect(image().value).toBe(value);
        expect(image().disabled).toBe(false);
    });
}

describe('Workspace image scope', () => {
    it.each([false, true])(
        'retains the last restored Files context across an interrupted reload (reject=%s)',
        async (reject) => {
            const fixture = imageScopeFixture();
            render(ImageScopeHarness, { fixture });
            await screen.findByRole('combobox', { name: 'Image' });
            await fireEvent.click(screen.getByRole('button', { name: 'Files' }));
            await screen.findByText('File 1', { selector: '.file-name span[title]' });
            await fireEvent.input(search(), { target: { value: 'File 1' } });
            await screen.findByText('/File 1', { selector: '.file-copy small' });
            await selectImage('source-2');
            let release!: () => void;
            fixture.setInventoryGate(async (member) => {
                if (member === 'source-1') {
                    await new Promise<void>((resolve) => {
                        release = resolve;
                    });
                    if (reject) throw new Error('Inventory unavailable');
                }
            });
            await selectImage('source-1');
            await waitFor(() => expect(release).toBeTypeOf('function'));
            if (reject) {
                release();
                await screen.findByRole('alert');
            }
            await selectImage('source-2');
            release();
            fixture.setInventoryGate();
            await selectImage('source-1');
            await screen.findByText('/File 1', { selector: '.file-copy small' });
            await waitFor(() => expect(search().value).toBe('File 1'));
        },
    );

    it('retains the originating document when Show in Files targets a member with another remembered interpretation', async () => {
        const fixture = imageScopeFixture(true);
        render(ImageScopeHarness, { fixture });
        await screen.findByRole('combobox', { name: 'Image' });
        await selectImage('source-4');
        await selectValue(screen.getByRole('combobox', { name: 'Native document' }), 'alternative');
        await waitFor(() => expect(fixture.workflow.documentId).toBe('alternative'));
        await selectImage('source-3');
        await fireEvent.click(screen.getByRole('button', { name: 'Waves' }));
        await fireEvent.click(await screen.findByRole('button', { name: 'Inspect Wave 20' }));
        await fireEvent.click(await screen.findByRole('button', { name: 'To Files' }));
        await screen.findByText('File 4', { selector: '.file-name span[title]' });
        expect(image().value).toBe('source-4');
        expect(fixture.workflow.documentId).toBe('document-tx16w');
        await waitFor(() =>
            expect(screen.getByRole('row', { selected: true }).getAttribute('data-file-entry')).toBe('source-4:file'),
        );
    });

    it('keeps Files navigation and search per image and restores the current image after refresh', async () => {
        render(ImageScopeHarness);
        await screen.findByRole('combobox', { name: 'Image' });
        expect(screen.getByText('Multiple')).toBeTruthy();
        await fireEvent.click(screen.getByRole('button', { name: 'Files' }));
        await screen.findByText('File 1', { selector: '.file-name span[title]' });
        const navigation = () => within(screen.getByRole('group', { name: 'Filesystem roots' }));
        expect(
            navigation()
                .getAllByRole('button')
                .map((item) => item.textContent?.trim()),
        ).toEqual(['Partition 1']);
        await fireEvent.input(search(), { target: { value: 'File' } });
        await screen.findByText('/File 1', { selector: '.file-copy small' });
        await selectImage('source-2');
        await fireEvent.click(screen.getByRole('button', { name: 'Files' }));
        await screen.findByText('File 2', { selector: '.file-name span[title]' });
        expect(navigation().queryByRole('button', { name: 'Partition 1' })).toBeNull();
        await selectImage('source-1');
        await screen.findByText('File 1', { selector: '.file-name span[title]' });
        await waitFor(() => expect(search().value).toBe('File'));
        await fireEvent.click(screen.getByRole('button', { name: 'Refresh fixture' }));
        await waitFor(() => expect(image().value).toBe('source-1'));
        expect(screen.queryByText('Files only')).toBeNull();
    });

    it('retains a declined or failed switch, disables blocked switching, and ejects the entire set', async () => {
        render(ImageScopeHarness);
        await screen.findByRole('combobox', { name: 'Image' });
        for (const label of ['Decline switch', 'Fail switch']) {
            await fireEvent.click(screen.getByRole('checkbox', { name: label }));
            await selectValue(image(), 'source-2');
            await waitFor(() => expect(image().value).toBe('source-1'));
            await fireEvent.click(screen.getByRole('checkbox', { name: label }));
        }
        await fireEvent.click(screen.getByRole('checkbox', { name: 'Block switching' }));
        expect(image().disabled).toBe(true);
        await fireEvent.click(screen.getByRole('checkbox', { name: 'Block switching' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Eject all images' }));
        await waitFor(() => expect(screen.getByLabelText('Fixture closed').textContent).toBe('true'));
        await waitFor(() => expect(screen.queryByRole('combobox', { name: 'Image' })).toBeNull());
    });

    it('isolates companion Files layouts but retains their shared native document and handles an unrecognized image', async () => {
        render(ImageScopeHarness);
        await screen.findByRole('combobox', { name: 'Image' });
        await selectImage('source-3');
        await fireEvent.click(screen.getByRole('button', { name: 'Waves' }));
        await screen.findByRole('button', { name: 'Inspect Wave 20' });
        await fireEvent.click(screen.getByRole('button', { name: 'Inspect Wave 20' }));
        await fireEvent.input(screen.getByRole('searchbox', { name: 'Search Waves' }), {
            target: { value: 'Wave 2' },
        });
        await selectImage('source-4');
        expect((screen.getByRole('searchbox', { name: 'Search Waves' }) as HTMLInputElement).value).toBe('Wave 2');
        await fireEvent.click(screen.getByRole('button', { name: 'Files' }));
        await screen.findByText('File 4', { selector: '.file-name span[title]' });
        await fireEvent.click(screen.getByRole('button', { name: 'Library panel' }));
        expect(screen.getByRole('button', { name: 'Library panel' }).getAttribute('aria-pressed')).toBe('false');
        await selectImage('source-3');
        await fireEvent.click(screen.getByRole('button', { name: 'Files' }));
        await screen.findByText('File 3', { selector: '.file-name span[title]' });
        expect(screen.getByRole('button', { name: 'Library panel' }).getAttribute('aria-pressed')).toBe('true');
        await selectImage('source-4');
        await screen.findByText('File 4', { selector: '.file-name span[title]' });
        expect(screen.getByRole('button', { name: 'Library panel' }).getAttribute('aria-pressed')).toBe('false');
        await selectImage('source-5');
        await screen.findByText('File 5', { selector: '.file-name span[title]' });
        expect((screen.getByRole('button', { name: 'Device' }) as HTMLButtonElement).disabled).toBe(true);
    });
});
