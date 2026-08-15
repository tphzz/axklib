import { fireEvent, render, screen, within } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import { programSampleSelectRows } from '../programSampleSelect';
import type { SamplerObject, SamplerRelationship } from '../transport';
import type { InspectorSelection, Program, ProgramAssignmentRow, SampleStructureItem } from '../types';
import ObjectEditor from './ObjectEditor.svelte';

function object(objectType: string, name: string): SamplerObject {
    return {
        key: `${objectType}-${name}`,
        objectType,
        name,
        partitionIndex: 0,
        partitionName: 'Partition 0',
        volumeName: 'Volume',
        categoryName: objectType,
        sfsId: 0,
        storedSizeBytes: 128,
        sampleRate: 44_100,
        rootKey: 60,
        frameCount: 44_100,
        sampleWidthBytes: 2,
    };
}

function programSelection(): Extract<InspectorSelection, { kind: 'program' }> {
    const programObject = object('PROG', '001');
    const program: Program = {
        id: programObject.key,
        objectId: programObject.key,
        slot: '001',
        name: 'Strings',
        object: programObject,
    };
    const relationship: SamplerRelationship = {
        id: 'assignment-1',
        sourceObjectId: program.objectId,
        targetObjectId: 'SBAC-String Bank',
        candidateObjectIds: [],
        relationshipType: 'PROG_ASSIGNMENT_TO_SBAC',
        quality: 'KNOWN',
        basis: 'test',
        notes: [],
        assignmentName: 'String Bank',
        assignmentState: 'stored-assignment',
        receiveChannelDisplay: 'A05',
    };
    const assignment: ProgramAssignmentRow = {
        relationship,
        targetObjectId: relationship.targetObjectId,
        targetType: 'SBAC',
        targetName: 'String Bank',
        confirmed: true,
    };
    const bankObject = object('SBAC', 'String Bank');
    const bank: SampleStructureItem = {
        id: bankObject.key,
        objectId: relationship.targetObjectId!,
        objectType: 'SBAC',
        object: bankObject,
        name: 'String Bank',
    };
    return {
        kind: 'program',
        program,
        assignments: [assignment],
        sampleSelect: programSampleSelectRows([assignment], [bank], []),
    };
}

function sampleSelection(): Extract<InspectorSelection, { kind: 'sample' }> {
    const sampleObject = object('SBNK', 'Violin C3');
    const item: SampleStructureItem = {
        id: sampleObject.key,
        objectId: sampleObject.key,
        objectType: 'SBNK',
        object: sampleObject,
        name: sampleObject.name,
    };
    return {
        kind: 'sample',
        item,
        memberships: [],
        preview: { item, waveData: [], preview: null, previewState: 'idle' },
    };
}

describe('ObjectEditor', () => {
    it('keeps Program assignments in Sample Select and exposes the complete Program tab set', async () => {
        const onassignmentselect = vi.fn();
        render(ObjectEditor, {
            props: {
                selection: programSelection(),
                assignmentQuery: '',
                onassignmentquerychange: vi.fn(),
                onassignmentselect,
            },
        });

        const expectedTabs = ['Sample Select', 'Easy Edit', 'Effect Setup', 'Setup', 'Control'];
        expect(screen.getAllByRole('tab').map((tab) => tab.textContent)).toEqual(expectedTabs);
        expect(screen.getByRole('tab', { name: 'Sample Select' }).getAttribute('aria-selected')).toBe('true');
        expect(screen.getByText('String Bank')).toBeTruthy();
        expect(screen.getByText('A05')).toBeTruthy();

        await fireEvent.click(screen.getByRole('button', { name: /String Bank/ }));
        expect(onassignmentselect).toHaveBeenCalledOnce();
        await fireEvent.click(screen.getByRole('tab', { name: 'Easy Edit' }));
        expect(screen.queryByText('String Bank')).toBeNull();
        expect(screen.getByRole('tabpanel', { name: 'Easy Edit' })).toBeTruthy();
    });

    it('does not present an unconfirmed stored Program row as an assignment', async () => {
        const onassignmentselect = vi.fn();
        const selection = programSelection();
        selection.assignments[0] = {
            ...selection.assignments[0],
            confirmed: false,
            relationship: {
                ...selection.assignments[0].relationship,
                quality: 'LIKELY',
            },
        };
        selection.sampleSelect = programSampleSelectRows(selection.assignments, [], []);
        render(ObjectEditor, {
            props: {
                selection,
                assignmentQuery: '',
                onassignmentquerychange: vi.fn(),
                onassignmentselect,
            },
        });

        expect(screen.queryByRole('button', { name: /String Bank/ })).toBeNull();
        expect(screen.getByText('No assigned Sample Banks or Samples')).toBeTruthy();
        expect(onassignmentselect).not.toHaveBeenCalled();
    });

    it('renders a missing named selector separately from the only resolved target', () => {
        const selection = programSelection();
        const astro = sampleItem('SBNK', 'Astro');
        const exact: ProgramAssignmentRow = {
            relationship: {
                id: 'assignment-astro',
                sourceObjectId: selection.program.objectId,
                targetObjectId: astro.objectId,
                candidateObjectIds: [astro.objectId],
                relationshipType: 'PROG_ASSIGNMENT_TO_SBNK',
                quality: 'KNOWN',
                basis: 'assignment-kind-0x10+name',
                notes: [],
                assignmentIndex: 0,
                assignmentName: 'Astro',
                assignmentState: 'stored-assignment',
                receiveChannelDisplay: '=Smp',
            },
            targetObjectId: astro.objectId,
            targetType: 'SBNK',
            targetName: 'Astro',
            confirmed: true,
        };
        const missing: ProgramAssignmentRow = {
            relationship: {
                id: 'assignment-missing',
                sourceObjectId: selection.program.objectId,
                candidateObjectIds: [],
                relationshipType: 'PROG_ASSIGNMENT_TO_SBNK',
                quality: 'UNKNOWN',
                basis: 'assignment-stored-missing-local-target',
                notes: [],
                assignmentIndex: 1,
                assignmentName: 'ASR10 MergeX   *',
                assignmentState: 'stored-assignment',
                receiveChannelDisplay: 'A01',
            },
            targetType: 'SBNK',
            targetName: 'ASR10 MergeX   *',
            confirmed: false,
        };
        selection.assignments = [exact, missing];
        selection.sampleSelect = programSampleSelectRows(selection.assignments, [], [astro]);

        render(ObjectEditor, {
            props: {
                selection,
                assignmentQuery: '',
                onassignmentquerychange: vi.fn(),
                onassignmentselect: vi.fn(),
            },
        });

        const table = screen.getByRole('table', { name: 'Program assignments' });
        const astroRow = within(table).getByRole('button', { name: /Astro/ });
        expect(astroRow.textContent?.replace(/\s+/g, ' ').trim()).toBe('AstroSBNK =Smp');
        expect(within(table).queryByText(/ASR10 MergeX/)).toBeNull();
    });

    it('shows the stored selector and status for source-load assignments', () => {
        const selection = programSelection();
        selection.assignments[0] = {
            ...selection.assignments[0],
            relationship: {
                ...selection.assignments[0].relationship,
                assignmentState: 'source-load-assignment',
                receiveChannelDisplay: '=Smp',
            },
        };
        selection.sampleSelect = programSampleSelectRows(selection.assignments, [], []);
        render(ObjectEditor, {
            props: {
                selection,
                assignmentQuery: '',
                onassignmentquerychange: vi.fn(),
                onassignmentselect: vi.fn(),
            },
        });

        expect(screen.getByText('Rch Assign')).toBeTruthy();
        expect(screen.getByText('=Smp')).toBeTruthy();
        expect(screen.getByText('Source load')).toBeTruthy();
        expect(
            screen.getByTitle('Stored CD-ROM selector; the sampler activates this assignment when it is loaded.'),
        ).toBeTruthy();
        expect(screen.queryByText('Program 001')).toBeNull();
        expect(screen.queryByText('Strings')).toBeNull();
    });

    it('defaults to assigned objects and expands to selector-ordered inventory with inactive rows last', async () => {
        const selection = programSelection();
        const bassBank = sampleItem('SBAC', 'Bass Bank');
        const cello = sampleItem('SBNK', 'Cello');
        const violin = sampleItem('SBNK', 'Violin');
        selection.sampleSelect = programSampleSelectRows(
            selection.assignments,
            [sampleItem('SBAC', 'String Bank', 'SBAC-String Bank'), bassBank],
            [violin, cello],
        );

        render(ObjectEditor, {
            props: {
                selection,
                assignmentQuery: '',
                onassignmentquerychange: vi.fn(),
                onassignmentselect: vi.fn(),
            },
        });

        const filter = screen.getByRole('checkbox', { name: 'Show only assigned' });
        expect((filter as HTMLInputElement).checked).toBe(true);
        expect(screen.getByText('1 item')).toBeTruthy();
        expect(screen.queryByText('Bass Bank')).toBeNull();

        await fireEvent.click(filter);
        expect(screen.getByText('4 items')).toBeTruthy();
        const rows = within(screen.getByRole('table', { name: 'Program assignments' })).getAllByRole('button');
        expect(rows.map((row) => row.textContent?.replace(/\s+/g, ' ').trim())).toEqual([
            'String BankSBAC A05',
            'Bass BankSBAC off',
            'CelloSBNK off',
            'ViolinSBNK off',
        ]);
    });

    it('keeps the expanded mode across Program changes and navigates unassigned inventory rows', async () => {
        const onassignmentselect = vi.fn();
        const selection = programSelection();
        const cello = sampleItem('SBNK', 'Cello');
        selection.sampleSelect = programSampleSelectRows(
            selection.assignments,
            [sampleItem('SBAC', 'String Bank', 'SBAC-String Bank')],
            [cello],
        );
        const rendered = render(ObjectEditor, {
            props: {
                selection,
                assignmentQuery: '',
                onassignmentquerychange: vi.fn(),
                onassignmentselect,
            },
        });

        await fireEvent.click(screen.getByRole('checkbox', { name: 'Show only assigned' }));
        await fireEvent.click(screen.getByRole('button', { name: /Cello/ }));
        expect(onassignmentselect).toHaveBeenLastCalledWith(
            expect.objectContaining({ targetObjectId: cello.objectId }),
        );

        await rendered.rerender({
            selection: { ...selection, program: { ...selection.program, name: 'Strings 2' } },
            assignmentQuery: '',
            onassignmentquerychange: vi.fn(),
            onassignmentselect,
        });
        expect((screen.getByRole('checkbox', { name: 'Show only assigned' }) as HTMLInputElement).checked).toBe(false);
        expect(screen.getByText('Cello')).toBeTruthy();
    });

    it('moves through navigable Sample Select rows with vertical navigation keys', async () => {
        const onassignmentselect = vi.fn();
        const selection = programSelection();
        const cello = sampleItem('SBNK', 'Cello');
        selection.sampleSelect = programSampleSelectRows(
            selection.assignments,
            [sampleItem('SBAC', 'String Bank', 'SBAC-String Bank')],
            [cello],
        );
        render(ObjectEditor, {
            props: {
                selection,
                assignmentQuery: '',
                onassignmentquerychange: vi.fn(),
                onassignmentselect,
            },
        });

        await fireEvent.click(screen.getByRole('checkbox', { name: 'Show only assigned' }));
        const stringBank = screen.getByRole('button', { name: /String Bank/ });
        stringBank.focus();
        await fireEvent.keyDown(stringBank, { key: 'ArrowDown' });

        expect(onassignmentselect).toHaveBeenLastCalledWith(expect.objectContaining({ targetName: 'Cello' }));
        expect(document.activeElement).toBe(screen.getByRole('button', { name: /Cello/ }));
    });

    it('pages through the complete Sample Select list while retaining focus', async () => {
        const onassignmentselect = vi.fn();
        const selection = programSelection();
        const samples = Array.from({ length: 8 }, (_, index) => sampleItem('SBNK', `Sample ${index + 1}`));
        selection.sampleSelect = programSampleSelectRows(
            selection.assignments,
            [sampleItem('SBAC', 'String Bank', 'SBAC-String Bank')],
            samples,
        );
        render(ObjectEditor, {
            props: {
                selection,
                assignmentQuery: '',
                onassignmentquerychange: vi.fn(),
                onassignmentselect,
            },
        });

        await fireEvent.click(screen.getByRole('checkbox', { name: 'Show only assigned' }));
        const first = screen.getByRole('button', { name: /String Bank/ });
        const list = first.closest<HTMLElement>('[data-navigation-list]')!;
        Object.defineProperty(list, 'clientHeight', { configurable: true, value: 120 });
        Object.defineProperty(first, 'offsetHeight', { configurable: true, value: 30 });
        first.focus();

        await fireEvent.keyDown(first, { key: 'PageDown' });

        expect(onassignmentselect).toHaveBeenLastCalledWith(expect.objectContaining({ targetName: 'Sample 3' }));
        expect(document.activeElement).toBe(screen.getByRole('button', { name: /Sample 3/ }));
    });

    it('exposes the complete SBNK tab set and remembers the active tab across SBNK selections', async () => {
        const { container, rerender } = render(ObjectEditor, {
            props: {
                selection: sampleSelection(),
                assignmentQuery: '',
                onassignmentquerychange: vi.fn(),
                onassignmentselect: vi.fn(),
            },
        });

        expect(screen.getAllByRole('tab').map((tab) => tab.textContent)).toEqual([
            'Trim/Loop',
            'Map/Out',
            'Filter/EG',
            'LFO',
            'MIDI/CTRL',
        ]);
        expect(container.querySelector('.editor-header > .editor-tabs:first-child')).toBeTruthy();
        expect(container.querySelector('.editor-object-title')).toBeNull();
        await fireEvent.click(screen.getByRole('tab', { name: 'LFO' }));
        await rerender({
            selection: sampleSelection(),
            assignmentQuery: '',
            onassignmentquerychange: vi.fn(),
            onassignmentselect: vi.fn(),
        });
        expect(screen.getByRole('tab', { name: 'LFO' }).getAttribute('aria-selected')).toBe('true');
    });

    it('uses neutral placeholders for unsupported editor selections', () => {
        const bankObject = object('SBAC', 'String Bank');
        const bank: SampleStructureItem = {
            id: bankObject.key,
            objectId: bankObject.key,
            objectType: 'SBAC',
            object: bankObject,
            name: bankObject.name,
        };
        render(ObjectEditor, {
            props: {
                selection: {
                    kind: 'sample-bank',
                    item: bank,
                    members: [],
                    memberPreviews: [],
                    displayedMemberId: '',
                },
                assignmentQuery: '',
                onassignmentquerychange: vi.fn(),
                onassignmentselect: vi.fn(),
            },
        });

        expect(screen.getByText('Sample Bank editor unavailable')).toBeTruthy();
        expect(screen.queryByRole('tab')).toBeNull();
    });
});

function sampleItem(
    objectType: 'SBAC' | 'SBNK',
    name: string,
    objectId = `${objectType}-${name}`,
): SampleStructureItem {
    const samplerObject = object(objectType, name);
    return {
        id: objectId,
        objectId,
        objectType,
        object: samplerObject,
        name,
    };
}
