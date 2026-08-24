/// <reference types="node" />

import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

import { fireEvent, render, screen, within } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import ImportDestinationChooser from './ImportDestinationChooser.svelte';

const appStyles = readFileSync(resolve(process.cwd(), 'src/app.css'), 'utf8');
const destinationSource = readFileSync(
    resolve(process.cwd(), 'src/lib/components/ImportDestinationChooser.svelte'),
    'utf8',
);

const partitions = [
    { partitionIndex: 0, name: 'SOUNDS 01' },
    { partitionIndex: 1, name: 'SOUNDS 02' },
];

const volumes = [
    { partitionIndex: 0, name: 'SOUNDS 01', volumeName: 'Pianos', label: 'SOUNDS 01 / Pianos' },
    { partitionIndex: 0, name: 'SOUNDS 01', volumeName: 'Strings', label: 'SOUNDS 01 / Strings' },
    { partitionIndex: 1, name: 'SOUNDS 02', volumeName: 'Pianos', label: 'SOUNDS 02 / Pianos' },
];

function props(overrides: Record<string, unknown> = {}) {
    return {
        mode: 'existing' as const,
        partitionIndex: 0,
        volumeName: 'Strings',
        partitions,
        volumes,
        disabled: false,
        onmode: vi.fn(),
        onvolume: vi.fn(),
        onpartition: vi.fn(),
        onname: vi.fn(),
        ...overrides,
    };
}

describe('ImportDestinationChooser', () => {
    it('uses one compact destination-volume row with concise mode labels', () => {
        render(ImportDestinationChooser, { props: props() });

        expect(screen.getByText('Destination volume')).toBeTruthy();
        expect(screen.getByRole('button', { name: 'Existing' })).toBeTruthy();
        expect(screen.getByRole('button', { name: 'New' })).toBeTruthy();
        expect(screen.getByRole('combobox', { name: 'Destination partition' })).toBeTruthy();
        expect(screen.getByRole('combobox', { name: 'Destination volume' })).toBeTruthy();
        expect(destinationSource).toMatch(
            /\.import-destination\s*\{[^}]*grid-template-columns:\s*max-content max-content minmax\([^;]+\) minmax\(0, 1fr\)/s,
        );
        const destinationRule = destinationSource.match(/\.import-destination\s*\{[^}]*\}/s)?.[0];
        expect(destinationRule).not.toContain('margin-bottom');
    });

    it('preselects the exact volume and exposes only volumes from the selected partition', async () => {
        render(ImportDestinationChooser, { props: props() });

        const combobox = screen.getByRole('combobox', { name: 'Destination volume' }) as HTMLInputElement;
        expect(combobox.value).toBe('Strings');
        await fireEvent.focus(combobox);

        expect(screen.getByRole('listbox', { name: 'Volumes' })).toBeTruthy();
        const options = within(screen.getByRole('listbox', { name: 'Volumes' }));
        expect(options.getAllByRole('option')).toHaveLength(2);
        expect(options.getByRole('option', { name: 'Pianos' })).toBeTruthy();
        expect(options.queryByRole('option', { name: /SOUNDS 02/ })).toBeNull();
    });

    it('filters volumes and selects an exact destination with the keyboard', async () => {
        const onvolume = vi.fn();
        render(ImportDestinationChooser, { props: props({ volumeName: '', onvolume }) });

        const combobox = screen.getByRole('combobox', { name: 'Destination volume' });
        await fireEvent.input(combobox, { target: { value: 'piano' } });
        expect(onvolume).toHaveBeenLastCalledWith(0, '');
        expect(within(screen.getByRole('listbox', { name: 'Volumes' })).getAllByRole('option')).toHaveLength(1);
        await fireEvent.keyDown(combobox, { key: 'ArrowDown' });
        await fireEvent.keyDown(combobox, { key: 'Enter' });

        expect(onvolume).toHaveBeenLastCalledWith(0, 'Pianos');
    });

    it('clears the selected volume, reopens all options, and retains keyboard focus', async () => {
        const onvolume = vi.fn();
        render(ImportDestinationChooser, { props: props({ onvolume }) });

        await fireEvent.click(screen.getByRole('button', { name: 'Clear volume' }));

        const combobox = screen.getByRole('combobox', { name: 'Destination volume' }) as HTMLInputElement;
        expect(onvolume).toHaveBeenLastCalledWith(0, '');
        expect(combobox.value).toBe('');
        expect(document.activeElement).toBe(combobox);
        expect(within(screen.getByRole('listbox', { name: 'Volumes' })).getAllByRole('option')).toHaveLength(2);
    });

    it('reports partition changes separately and disables volume selection for an empty partition', async () => {
        const onpartition = vi.fn();
        render(ImportDestinationChooser, {
            props: props({ partitionIndex: 1, volumeName: '', volumes: volumes.slice(0, 2), onpartition }),
        });

        const partition = screen.getByRole('combobox', { name: 'Destination partition' });
        await fireEvent.change(partition, { target: { value: '0' } });
        expect(onpartition).toHaveBeenCalledWith(0);

        const volume = screen.getByRole('combobox', { name: 'Destination volume' }) as HTMLInputElement;
        expect(volume.disabled).toBe(true);
        expect(volume.placeholder).toBe('No volumes in this partition');
    });

    it('uses the same dark dialog control treatment for a new volume name', async () => {
        render(ImportDestinationChooser, {
            props: props({ mode: 'create', partitionIndex: 0, volumeName: '' }),
        });

        expect(screen.getByRole('textbox', { name: 'New volume name' }).classList).toContain('dialog-field-control');
        expect(screen.getByRole('combobox', { name: 'Destination partition' }).classList).toContain(
            'dialog-field-control',
        );
    });

    it('uses the shared compact dialog typography and dark autocomplete palette', async () => {
        render(ImportDestinationChooser, { props: props() });

        const combobox = screen.getByRole('combobox', { name: 'Destination volume' });
        await fireEvent.focus(combobox);
        const listbox = screen.getByRole('listbox', { name: 'Volumes' });
        const option = within(listbox).getAllByRole('option')[0];

        expect(combobox.classList).toContain('dialog-field-control');
        expect(listbox.classList).toContain('dialog-autocomplete-list');
        expect(option.classList).toContain('dialog-autocomplete-option');

        const controlRule = appStyles.match(/\.dialog-field-control\s*\{[^}]+\}/)?.[0];
        const listRule = appStyles.match(/\.dialog-autocomplete-list\s*\{[^}]+\}/)?.[0];
        const optionRule = appStyles.match(/\.dialog-autocomplete-option\s*\{[^}]+\}/)?.[0];
        expect(controlRule).toContain('font-size: var(--dialog-control-font-size)');
        expect(listRule).toContain('background: var(--color-bg-deep)');
        expect(optionRule).toContain('font-size: var(--dialog-control-font-size)');
        expect(appStyles).toContain('--density-control: 26px');
        expect(appStyles).toContain('--dialog-control-font-size: 11px');
        expect(appStyles).toContain('--color-bg-deep: #0b0d0f');

        const style = document.createElement('style');
        style.textContent = `${controlRule}\n${listRule}\n${optionRule}`
            .replaceAll('var(--density-control)', '26px')
            .replaceAll('var(--dialog-control-font-size)', '11px')
            .replaceAll('var(--color-bg-deep)', '#0b0d0f');
        document.head.append(style);

        const controlStyle = getComputedStyle(combobox);
        const listStyle = getComputedStyle(listbox);
        const optionStyle = getComputedStyle(option);
        expect(controlStyle.height).toBe('26px');
        expect(controlStyle.fontSize).toBe('11px');
        expect(listStyle.backgroundColor).toBe('rgb(11, 13, 15)');
        expect(optionStyle.minHeight).toBe('28px');
        expect(optionStyle.fontSize).toBe('11px');

        style.remove();
    });
});
