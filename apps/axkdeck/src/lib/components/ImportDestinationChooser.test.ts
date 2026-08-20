/// <reference types="node" />

import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import ImportDestinationChooser from './ImportDestinationChooser.svelte';

const appStyles = readFileSync(resolve(process.cwd(), 'src/app.css'), 'utf8');

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
    it('preselects the exact volume and exposes every partition volume through an autocomplete', async () => {
        render(ImportDestinationChooser, { props: props() });

        const combobox = screen.getByRole('combobox', { name: 'Volume' }) as HTMLInputElement;
        expect(combobox.value).toBe('SOUNDS 01 / Strings');
        await fireEvent.focus(combobox);

        expect(screen.getByRole('listbox', { name: 'Volumes' })).toBeTruthy();
        expect(screen.getAllByRole('option')).toHaveLength(3);
        expect(screen.getByRole('option', { name: /SOUNDS 02.*Pianos/ })).toBeTruthy();
    });

    it('filters volumes and selects an exact destination with the keyboard', async () => {
        const onvolume = vi.fn();
        render(ImportDestinationChooser, { props: props({ partitionIndex: null, volumeName: '', onvolume }) });

        const combobox = screen.getByRole('combobox', { name: 'Volume' });
        await fireEvent.input(combobox, { target: { value: 'sounds 02' } });
        expect(onvolume).toHaveBeenLastCalledWith(null, '');
        expect(screen.getAllByRole('option')).toHaveLength(1);
        await fireEvent.keyDown(combobox, { key: 'ArrowDown' });
        await fireEvent.keyDown(combobox, { key: 'Enter' });

        expect(onvolume).toHaveBeenLastCalledWith(1, 'Pianos');
    });

    it('clears the selected volume, reopens all options, and retains keyboard focus', async () => {
        const onvolume = vi.fn();
        render(ImportDestinationChooser, { props: props({ onvolume }) });

        await fireEvent.click(screen.getByRole('button', { name: 'Clear volume' }));

        const combobox = screen.getByRole('combobox', { name: 'Volume' }) as HTMLInputElement;
        expect(onvolume).toHaveBeenLastCalledWith(null, '');
        expect(combobox.value).toBe('');
        expect(document.activeElement).toBe(combobox);
        expect(screen.getAllByRole('option')).toHaveLength(3);
    });

    it('uses the same dark dialog control treatment for a new volume name', async () => {
        render(ImportDestinationChooser, {
            props: props({ mode: 'create', partitionIndex: 0, volumeName: '' }),
        });

        expect(screen.getByRole('textbox', { name: 'Volume name' }).classList).toContain('dialog-field-control');
        expect(screen.getByRole('combobox', { name: 'Partition' }).classList).toContain('dialog-field-control');
    });

    it('uses the shared compact dialog typography and dark autocomplete palette', async () => {
        render(ImportDestinationChooser, { props: props() });

        const combobox = screen.getByRole('combobox', { name: 'Volume' });
        await fireEvent.focus(combobox);
        const listbox = screen.getByRole('listbox', { name: 'Volumes' });
        const option = screen.getAllByRole('option')[0];

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
