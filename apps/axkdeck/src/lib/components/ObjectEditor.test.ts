import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
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
        assignmentState: 'confirmed-active',
        receiveChannelDisplay: '05',
    };
    const assignment: ProgramAssignmentRow = {
        relationship,
        targetObjectId: relationship.targetObjectId,
        targetType: 'SBAC',
        targetName: 'String Bank',
        confirmed: true,
    };
    return { kind: 'program', program, assignments: [assignment] };
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
        expect(screen.getByText('05')).toBeTruthy();

        await fireEvent.click(screen.getByRole('button', { name: /String Bank/ }));
        expect(onassignmentselect).toHaveBeenCalledOnce();
        await fireEvent.click(screen.getByRole('tab', { name: 'Easy Edit' }));
        expect(screen.queryByText('String Bank')).toBeNull();
        expect(screen.getByRole('tabpanel', { name: 'Easy Edit' })).toBeTruthy();
    });

    it('shows unconfirmed Program assignments without making them navigable', async () => {
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
        render(ObjectEditor, {
            props: {
                selection,
                assignmentQuery: '',
                onassignmentquerychange: vi.fn(),
                onassignmentselect,
            },
        });

        const assignment = screen.getByRole('button', { name: /String Bank/ });
        expect(assignment.hasAttribute('disabled')).toBe(true);
        expect(screen.getByText('Unconfirmed assignment')).toBeTruthy();
        await fireEvent.click(assignment);
        expect(onassignmentselect).not.toHaveBeenCalled();
    });

    it('shows the stored selector and status for source-load assignments', () => {
        const selection = programSelection();
        selection.assignments[0] = {
            ...selection.assignments[0],
            relationship: {
                ...selection.assignments[0].relationship,
                assignmentState: 'source-load-assignment',
                receiveChannelDisplay: '=SMP',
            },
        };
        render(ObjectEditor, {
            props: {
                selection,
                assignmentQuery: '',
                onassignmentquerychange: vi.fn(),
                onassignmentselect: vi.fn(),
            },
        });

        expect(screen.getByText('Rch Assign')).toBeTruthy();
        expect(screen.getByText('=SMP')).toBeTruthy();
        expect(screen.getByText('Source load')).toBeTruthy();
        expect(
            screen.getByTitle('Stored CD-ROM selector; the sampler activates this assignment when it is loaded.'),
        ).toBeTruthy();
        expect(screen.queryByText('Program 001')).toBeNull();
        expect(screen.queryByText('Strings')).toBeNull();
    });

    it('exposes the complete SBNK tab set and remembers the active tab across SBNK selections', async () => {
        const { rerender } = render(ObjectEditor, {
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
