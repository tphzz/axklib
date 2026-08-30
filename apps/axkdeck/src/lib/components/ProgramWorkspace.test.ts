import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

import { fireEvent, render, screen, within } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import type { SamplerObject, SystemProgramContexts } from '../transport';
import type { Program } from '../types';
import ProgramWorkspace from './ProgramWorkspace.svelte';

const appStyles = readFileSync(resolve(process.cwd(), 'src/app.css'), 'utf8');
const objectSizeIdentitySource = readFileSync(
    resolve(process.cwd(), 'src/lib/components/ObjectSizeIdentity.svelte'),
    'utf8',
);

function program(programNumber: number, name: string): Program {
    const slot = String(programNumber).padStart(3, '0');
    const object: SamplerObject = {
        key: `program-${slot}`,
        objectType: 'PROG',
        name: slot,
        partitionIndex: 0,
        partitionName: 'Partition 0',
        volumeName: 'Volume',
        categoryName: 'PROG',
        objectEncoding: 'current',
        directoryEntryName: `PROG.${slot}`,
        sfsId: programNumber,
        storedSizeBytes: 128,
        sizeWithDependenciesBytes: null,
        sampleRate: 0,
        rootKey: 0,
        frameCount: 0,
        sampleWidthBytes: 0,
    };
    return { id: object.key, objectId: object.key, slot, programNumber, name, object };
}

function programWithDependencies(programNumber: number, name: string, sizeWithDependenciesBytes: number): Program {
    const result = program(programNumber, name);
    return { ...result, object: { ...result.object, sizeWithDependenciesBytes } };
}

const system2 = {
    fileKind: 'SYSTEM2' as const,
    availability: 'AVAILABLE' as const,
    model: 'A5000' as const,
    savedProgramMode: 'SINGLE' as const,
    basicReceive: { port: 'A' as const, channel: 1, display: 'A01' },
    omni: false,
    programChangeEnabled: true,
    parts: [
        {
            partNumber: 1,
            partLabel: 'A01',
            midi: { port: 'A' as const, channel: 1, display: 'A01' },
            programNumber: 3,
            master: true,
        },
        {
            partNumber: 2,
            partLabel: 'A02',
            midi: { port: 'A' as const, channel: 2, display: 'A02' },
            programNumber: 3,
            master: false,
        },
        {
            partNumber: 17,
            partLabel: 'B01',
            midi: { port: 'B' as const, channel: 1, display: 'B01' },
            programNumber: 9,
            master: false,
        },
    ],
};

const a3000 = {
    fileKind: 'SYSTEM' as const,
    availability: 'AVAILABLE' as const,
    model: 'A3000' as const,
    basicReceive: { port: 'A' as const, channel: 16, display: '16' },
    omni: true,
    programChangeEnabled: false,
};

const contexts: SystemProgramContexts = {
    partitionIndex: 0,
    files: [a3000, system2],
    message: '',
};

const baseProps = {
    programs: [program(3, 'Bass')],
    contexts,
    contextsLoading: false,
    contextsError: '',
    presentation: 'multi' as const,
    selectedPartNumber: null,
    activeObjectId: '',
    query: '',
    onquerychange: vi.fn(),
    onpresentationchange: vi.fn(),
    onprogramselect: vi.fn(),
    onpartselect: vi.fn(),
};

describe('ProgramWorkspace', () => {
    it('keeps presentation controls compact and moves saved System File details into the title popover', async () => {
        const onpresentationchange = vi.fn();
        render(ProgramWorkspace, { props: { ...baseProps, presentation: 'single', onpresentationchange } });

        const single = screen.getByRole('button', { name: 'Single Program view' });
        const multi = screen.getByRole('button', { name: 'Multi Part view' });
        expect(single.getAttribute('aria-pressed')).toBe('true');
        expect(single.querySelector('[data-icon="program-single"]')).toBeTruthy();
        expect(multi.querySelector('[data-icon="program-multi"]')).toBeTruthy();
        expect(screen.queryByText(/SYSTEM2 · A5000 · Basic Rch A01/)).toBeNull();

        const details = screen.getByRole('button', { name: 'Saved System File details' });
        expect(details.getAttribute('aria-expanded')).toBe('false');
        expect(details.closest('.collection-title')).toBeTruthy();
        await fireEvent.mouseEnter(details.closest('.program-system-info')!);
        const popover = screen.getByRole('dialog', { name: 'Saved System File details' });
        expect(details.getAttribute('aria-expanded')).toBe('true');
        expect(within(popover).getByText('SYSTEM')).toBeTruthy();
        expect(within(popover).getByText('SYSTEM2')).toBeTruthy();
        expect(within(popover).getByText('A5000')).toBeTruthy();
        expect(within(popover).getByText('A01')).toBeTruthy();
        expect(within(popover).getByText('Single')).toBeTruthy();
        await fireEvent.mouseLeave(details.closest('.program-system-info')!);
        expect(screen.queryByRole('dialog', { name: 'Saved System File details' })).toBeNull();
        await fireEvent.focus(details);
        expect(screen.getByRole('dialog', { name: 'Saved System File details' })).toBeTruthy();
        await fireEvent.blur(details);
        expect(screen.queryByRole('dialog', { name: 'Saved System File details' })).toBeNull();

        await fireEvent.click(multi);
        expect(onpresentationchange).toHaveBeenCalledWith('multi');
    });

    it('pins the System File popover on click and closes it with Escape or an outside click', async () => {
        render(ProgramWorkspace, { props: baseProps });

        const details = screen.getByRole('button', { name: 'Saved System File details' });
        await fireEvent.click(details);
        expect(screen.getByRole('dialog', { name: 'Saved System File details' })).toBeTruthy();
        await fireEvent.mouseLeave(details.closest('.program-system-info')!);
        expect(screen.getByRole('dialog', { name: 'Saved System File details' })).toBeTruthy();

        await fireEvent.keyDown(window, { key: 'Escape' });
        expect(screen.queryByRole('dialog', { name: 'Saved System File details' })).toBeNull();
        expect(document.activeElement).toBe(details);

        await fireEvent.click(details);
        await fireEvent.click(document.body);
        expect(screen.queryByRole('dialog', { name: 'Saved System File details' })).toBeNull();
    });

    it('renders compact Part, Program, and Role columns without duplicating the fixed MIDI address', async () => {
        const onpartselect = vi.fn();
        render(ProgramWorkspace, { props: { ...baseProps, onpartselect } });

        expect(screen.queryByText('Saved System File mode: Single')).toBeNull();
        expect(screen.queryByText(/Multi Part routing is authoritative/)).toBeNull();
        const table = screen.getByRole('region', { name: 'Multi Part assignments' });
        for (const heading of ['Part', 'Program', 'Role']) expect(within(table).getByText(heading)).toBeTruthy();
        expect(within(table).queryByText('MIDI')).toBeNull();
        const a01 = screen.getByRole('button', { name: 'Part A01, 003: Bass, Master' });
        expect(a01.textContent?.replace(/\s+/g, ' ').trim()).toBe('A01 003: Bass Master');
        expect(screen.getByRole('button', { name: 'Part B01, 009: Pgm 009' })).toBeTruthy();

        await fireEvent.click(screen.getByRole('button', { name: /Part B01/ }));
        expect(onpartselect).toHaveBeenCalledWith(system2.parts[2], null);
    });

    it('uses direct one-based SYSTEM2 Program numbers without shifting the selected Program', async () => {
        const assignedProgram = program(1, 'Strings1');
        const directPart = { ...system2.parts[0], programNumber: 1 };
        const directContexts: SystemProgramContexts = {
            ...contexts,
            files: [a3000, { ...system2, parts: [directPart] }],
        };
        const onpartselect = vi.fn();
        render(ProgramWorkspace, {
            props: { ...baseProps, programs: [assignedProgram], contexts: directContexts, onpartselect },
        });

        const a01 = screen.getByRole('button', { name: 'Part A01, 001: Strings1, Master' });
        await fireEvent.click(a01);

        expect(onpartselect).toHaveBeenCalledWith(directPart, assignedProgram);
    });

    it('uses the shared list cursor keys across the Multi Part table', async () => {
        const onpartselect = vi.fn();
        render(ProgramWorkspace, { props: { ...baseProps, onpartselect } });

        const a01 = screen.getByRole('button', { name: /Part A01/ });
        const list = a01.closest<HTMLElement>('[data-navigation-list]')!;
        Object.defineProperty(list, 'clientHeight', { configurable: true, value: 90 });
        Object.defineProperty(a01, 'offsetHeight', { configurable: true, value: 30 });
        a01.focus();

        await fireEvent.keyDown(a01, { key: 'ArrowDown' });
        expect(onpartselect).toHaveBeenLastCalledWith(system2.parts[1], program(3, 'Bass'));
        expect(document.activeElement).toBe(screen.getByRole('button', { name: /Part A02/ }));

        await fireEvent.keyDown(document.activeElement!, { key: 'End' });
        expect(onpartselect).toHaveBeenLastCalledWith(system2.parts[2], null);
        expect(document.activeElement).toBe(screen.getByRole('button', { name: /Part B01/ }));

        await fireEvent.keyDown(document.activeElement!, { key: 'Home' });
        expect(onpartselect).toHaveBeenLastCalledWith(system2.parts[0], program(3, 'Bass'));
        expect(document.activeElement).toBe(a01);

        await fireEvent.keyDown(a01, { key: 'PageDown' });
        expect(onpartselect).toHaveBeenLastCalledWith(system2.parts[2], null);
        expect(document.activeElement).toBe(screen.getByRole('button', { name: /Part B01/ }));
    });

    it('navigates only the Multi Parts retained by search', async () => {
        const onpartselect = vi.fn();
        render(ProgramWorkspace, { props: { ...baseProps, query: '01', onpartselect } });

        expect(screen.queryByRole('button', { name: /Part A02/ })).toBeNull();
        const a01 = screen.getByRole('button', { name: /Part A01/ });
        a01.focus();
        await fireEvent.keyDown(a01, { key: 'ArrowDown' });

        expect(onpartselect).toHaveBeenLastCalledWith(system2.parts[2], null);
        expect(document.activeElement).toBe(screen.getByRole('button', { name: /Part B01/ }));
    });

    it('matches the established Program list typography and density', () => {
        const rowRule = appStyles.match(/\.program-multi-table > button\s*\{[^}]+\}/)?.[0];
        const programRule = appStyles.match(/\.program-multi-program\s*\{[^}]+\}/)?.[0];
        const metadataRule = appStyles.match(
            /\.program-multi-part,\s*\.program-multi-program-slot,\s*\.program-multi-role\s*\{[^}]+\}/,
        )?.[0];
        const programNameRule = appStyles.match(/\.program-multi-program strong\s*\{[^}]+\}/)?.[0];

        expect(rowRule).toContain('min-height: var(--density-row)');
        expect(rowRule).toContain('padding: 2px 7px');
        expect(programRule).toContain('grid-template-columns: max-content minmax(0, 1fr)');
        expect(programRule).toContain('column-gap: 5px');
        expect(programNameRule).toContain('font-size: 10px');
        expect(metadataRule).toContain('font-size: 8.5px');
    });

    it('renders the active Single Program list with the shared compact two-line identity', () => {
        render(ProgramWorkspace, {
            props: {
                ...baseProps,
                programs: [programWithDependencies(1, 'REZO LD', 44 * 1024)],
                presentation: 'single',
            },
        });

        const row = document.querySelector('.program-row');
        expect(row?.querySelector('.object-slot')?.textContent).toBe('001');
        expect(row?.querySelector('.program-identity .object-size-primary')?.textContent).toContain('REZO LD');
        expect(row?.querySelector('.program-identity .object-size-secondary')?.textContent).toBe(
            '128 B · 44 KiB incl. deps.',
        );
        expect(row?.querySelector('.object-size-summary')).toBeNull();

        const primary = row?.querySelector('.object-size-primary strong');
        const secondary = row?.querySelector('.object-size-secondary');
        expect(primary).toBeTruthy();
        expect(secondary).toBeTruthy();

        const listRule = appStyles.match(/\.program-list\s*\{[^}]+\}/)?.[0];
        const rowRule = appStyles.match(/\.program-row\s*\{[^}]+\}/)?.[0];
        const slotRule = appStyles.match(/\.program-row \.object-slot\s*\{[^}]+\}/)?.[0];
        const identityRule = appStyles.match(/\.program-identity\s*\{[^}]+\}/)?.[0];
        const primaryRule = objectSizeIdentitySource.match(/\.object-size-primary strong\s*\{[^}]+\}/)?.[0];
        const secondaryRule = objectSizeIdentitySource.match(/\.object-size-secondary\s*\{[^}]+\}/)?.[0];
        expect(listRule).toContain('gap: 0');
        expect(listRule).toContain('padding: 2px 6px 5px');
        expect(rowRule).toContain('height: var(--density-row)');
        expect(rowRule).toContain('grid-template-columns: 30px minmax(0, 1fr)');
        expect(rowRule).toContain('gap: 4px');
        expect(rowRule).toContain('align-items: center');
        expect(rowRule).toContain('padding: 0');
        expect(rowRule).toContain('font: inherit');
        expect(slotRule).toContain('padding: 3px 6px 1px');
        expect(identityRule).toContain('min-height: calc(var(--density-row) - 2px)');
        expect(identityRule).toContain('padding: 2px 6px 2px 0');
        expect(identityRule).toContain('font-size: 10px');
        expect(identityRule).toContain('line-height: 10px');
        expect(primaryRule).toContain('font-size: 10px');
        expect(primaryRule).toContain('line-height: 10px');
        expect(secondaryRule).toContain('font-size: 8.5px');
        expect(secondaryRule).toContain('line-height: 9px');

        const style = document.createElement('style');
        style.textContent = `${listRule}\n${rowRule?.replaceAll('var(--density-row)', '26px')}\n${slotRule}\n${identityRule?.replaceAll('var(--density-row)', '26px')}\n${primaryRule}\n${secondaryRule}`;
        document.head.append(style);

        expect(getComputedStyle(row!).height).toBe('26px');
        expect(getComputedStyle(row!).paddingTop).toBe('0px');
        expect(getComputedStyle(row!).paddingBottom).toBe('0px');
        expect(getComputedStyle(row!.querySelector('.object-slot')!).paddingTop).toBe('3px');
        expect(getComputedStyle(row!.querySelector('.object-slot')!).paddingBottom).toBe('1px');
        expect(getComputedStyle(row!.querySelector('.program-identity')!).paddingTop).toBe('2px');
        expect(getComputedStyle(row!.querySelector('.program-identity')!).paddingRight).toBe('6px');
        expect(getComputedStyle(row!.querySelector('.program-identity')!).paddingBottom).toBe('2px');
        expect(getComputedStyle(row!.querySelector('.program-identity')!).paddingLeft).toBe('0px');
        expect(getComputedStyle(primary!).fontSize).toBe('10px');
        expect(getComputedStyle(primary!).lineHeight).toBe('10px');
        expect(getComputedStyle(secondary!).fontSize).toBe('8.5px');
        expect(getComputedStyle(secondary!).lineHeight).toBe('9px');

        style.remove();
    });

    it('tracks the clicked part independently when two parts use the same Program', () => {
        render(ProgramWorkspace, { props: { ...baseProps, selectedPartNumber: 2 } });

        expect(screen.getByRole('button', { name: /Part A01/ }).getAttribute('aria-pressed')).toBe('false');
        expect(screen.getByRole('button', { name: /Part A02/ }).getAttribute('aria-pressed')).toBe('true');
    });

    it('keeps a valid SYSTEM2 usable and exposes an invalid SYSTEM only in the popover', async () => {
        render(ProgramWorkspace, {
            props: {
                ...baseProps,
                contexts: {
                    partitionIndex: 0,
                    files: [{ fileKind: 'SYSTEM', availability: 'INVALID', message: 'SYSTEM is malformed.' }, system2],
                    message: '',
                },
            },
        });
        expect(screen.getAllByRole('button', { name: /003: Bass/ })).toHaveLength(2);
        expect(screen.queryByText('SYSTEM is malformed.')).toBeNull();
        await fireEvent.click(screen.getByRole('button', { name: 'Saved System File details' }));
        expect(screen.getByText('SYSTEM is malformed.')).toBeTruthy();
    });

    it('explains that an A3000 SYSTEM context cannot provide Multi assignments', () => {
        render(ProgramWorkspace, {
            props: {
                ...baseProps,
                contexts: {
                    partitionIndex: 0,
                    files: [
                        a3000,
                        {
                            fileKind: 'SYSTEM2',
                            availability: 'NOT_PRESENT',
                            message: 'No saved SYSTEM2 file exists for partition 0.',
                        },
                    ],
                    message: '',
                },
            },
        });
        expect(
            screen.getByText(
                'A3000 SYSTEM stores receive settings, but it does not contain Program Mode or Multi Part assignments.',
            ),
        ).toBeTruthy();
    });

    it('reports an invalid SYSTEM2 instead of treating the A3000 context as a Multi source', () => {
        render(ProgramWorkspace, {
            props: {
                ...baseProps,
                contexts: {
                    partitionIndex: 0,
                    files: [a3000, { fileKind: 'SYSTEM2', availability: 'INVALID', message: 'SYSTEM2 is malformed.' }],
                    message: '',
                },
            },
        });
        expect(screen.getByText('SYSTEM2 is malformed.')).toBeTruthy();
        expect(screen.queryByText(/A3000 SYSTEM stores receive settings/)).toBeNull();
    });

    it('shows the collection-level unsupported-media explanation when no contexts exist', () => {
        const message = 'System File decoding is not supported for this media format.';
        render(ProgramWorkspace, {
            props: {
                ...baseProps,
                contexts: { partitionIndex: 0, files: [], message },
            },
        });
        expect(screen.getByText(message)).toBeTruthy();
    });

    it('orders cleanup before generation in Single view and hides both in Multi view', async () => {
        const cleanup = vi.fn();
        const generate = vi.fn();
        const rendered = render(ProgramWorkspace, {
            props: {
                ...baseProps,
                presentation: 'single',
                programAssignmentCleanupAvailable: true,
                onprogramassignmentcleanup: cleanup,
                programGenerationAvailable: true,
                onprogramgeneration: generate,
            },
        });
        const actions = screen
            .getAllByRole('button')
            .filter((button) =>
                ['Clean unresolved Program assignments', 'Generate Programs'].includes(
                    button.getAttribute('aria-label') ?? '',
                ),
            );
        expect(actions.map((button) => button.getAttribute('aria-label'))).toEqual([
            'Clean unresolved Program assignments',
            'Generate Programs',
        ]);
        expect(actions[0]?.textContent).not.toContain('Clean unresolved Program assignments');
        expect(actions[1]?.textContent).not.toContain('Generate Programs');
        await fireEvent.click(actions[0]!);
        await fireEvent.click(actions[1]!);
        expect(cleanup).toHaveBeenCalledOnce();
        expect(generate).toHaveBeenCalledOnce();

        await rendered.rerender({
            ...baseProps,
            presentation: 'multi',
            programAssignmentCleanupAvailable: true,
            programGenerationAvailable: true,
        });
        expect(screen.queryByRole('button', { name: 'Clean unresolved Program assignments' })).toBeNull();
        expect(screen.queryByRole('button', { name: 'Generate Programs' })).toBeNull();
    });
});
