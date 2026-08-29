import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import type { ProgramAssignmentCleanupInspection } from '../transport';
import ProgramAssignmentCleanupDialog from './ProgramAssignmentCleanupDialog.svelte';

const componentSource = readFileSync(
    resolve(process.cwd(), 'src/lib/components/ProgramAssignmentCleanupDialog.svelte'),
    'utf8',
);

const inspection: ProgramAssignmentCleanupInspection = {
    imageId: 'image-1',
    revision: 1,
    contentScopeId: 'volume-1',
    totalCandidateCount: 2,
    candidates: [
        {
            programObjectId: 'program-1',
            programNumber: 1,
            programName: 'Kit',
            assignmentOrdinal: 0,
            assignmentName: 'Missing Bank',
            targetObjectType: 'SBAC',
            receiveChannelDisplay: '=Smp',
            reason: 'MISSING_TARGET',
            candidateTargetCount: 0,
            defaultSelected: true,
        },
        {
            programObjectId: 'program-1',
            programNumber: 1,
            programName: 'Kit',
            assignmentOrdinal: 2,
            assignmentName: 'Old Snare',
            targetObjectType: 'SBNK',
            receiveChannelDisplay: 'A02',
            reason: 'AMBIGUOUS_TARGET',
            candidateTargetCount: 2,
            defaultSelected: true,
        },
    ],
};

describe('ProgramAssignmentCleanupDialog', () => {
    it('renders a compact hierarchy and exposes global, Program, and row selection', async () => {
        const onselectall = vi.fn();
        const onprogramselectionchange = vi.fn();
        const onselectionchange = vi.fn();
        render(ProgramAssignmentCleanupDialog, {
            props: {
                volumeName: 'Drums',
                inspection,
                rows: inspection.candidates.map((candidate) => ({ ...candidate, selected: true })),
                loading: false,
                busy: false,
                error: '',
                onselectall,
                onprogramselectionchange,
                onselectionchange,
                oncancel: vi.fn(),
                onconfirm: vi.fn(),
            },
        });

        expect(screen.getByText('001: Kit')).toBeTruthy();
        expect(screen.getByText('Missing target')).toBeTruthy();
        expect(screen.getByText('2 matching targets')).toBeTruthy();
        const listRule = componentSource.match(/\.program-assignment-cleanup-list\s*\{[^}]+\}/)?.[0];
        expect(listRule).toContain('scrollbar-gutter: stable');
        expect(listRule).toContain('padding-right: 14px');
        await fireEvent.click(screen.getByRole('checkbox', { name: 'Select all unresolved assignments' }));
        expect(onselectall).toHaveBeenCalledWith(false);
        await fireEvent.click(screen.getByRole('checkbox', { name: 'Select assignments for 001: Kit' }));
        expect(onprogramselectionchange).toHaveBeenCalledWith('program-1', false);
        await fireEvent.click(screen.getByRole('checkbox', { name: 'Select Assignment 3 Old Snare' }));
        expect(onselectionchange).toHaveBeenCalledWith('program-1', 2, false);
    });
});
