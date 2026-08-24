import { fireEvent, render, screen } from '@testing-library/svelte';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, expect, it, vi } from 'vitest';

import type { ProgramGenerationInspection } from '../transport';
import type { ProgramGenerationRow } from '../../features/program-generation/workflow.svelte';
import ProgramGenerationDialog from './ProgramGenerationDialog.svelte';

const dialogSource = readFileSync(resolve(process.cwd(), 'src/lib/components/ProgramGenerationDialog.svelte'), 'utf8');

const inspection: ProgramGenerationInspection = {
    imageId: 'image-1',
    revision: 2,
    contentScopeId: 'volume-1',
    availableProgramNumbers: [1],
    candidates: [],
    notices: [{ code: 'AMBIGUOUS_BANK', message: 'One Sample Bank was skipped.', objectIds: ['bank-2'] }],
};

const rows: ProgramGenerationRow[] = [
    {
        targetObjectId: 'bank-1',
        targetObjectName: 'Drum Bank',
        targetObjectType: 'SBAC',
        defaultProgramName: 'DrumBank',
        defaultSelected: true,
        selected: true,
        programName: 'DrumBank',
        programNumber: 1,
    },
    {
        targetObjectId: 'sample-1',
        targetObjectName: 'Kick',
        targetObjectType: 'SBNK',
        defaultProgramName: 'Kick',
        defaultSelected: false,
        selected: false,
        programName: 'Kick',
        programNumber: null,
    },
];

describe('ProgramGenerationDialog', () => {
    it('previews slots, targets, names, and the sampler receive assignment', async () => {
        const onselectionchange = vi.fn();
        const onnamechange = vi.fn();
        const onselectall = vi.fn();
        const onconfirm = vi.fn();
        const oncancel = vi.fn();
        render(ProgramGenerationDialog, {
            props: {
                volumeName: 'Drums',
                inspection,
                rows,
                loading: false,
                busy: false,
                error: '',
                onselectionchange,
                onnamechange,
                onselectall,
                onconfirm,
                oncancel,
            },
        });

        expect(screen.getByRole('dialog', { name: 'Generate Programs' })).toBeTruthy();
        expect(screen.getByText('Sample Bank')).toBeTruthy();
        expect(screen.getByText('Sample')).toBeTruthy();
        expect(screen.getAllByText('=Smp')).toHaveLength(2);
        expect(screen.getByText('001')).toBeTruthy();
        expect(screen.getByText('No free slot')).toBeTruthy();

        const name = screen.getByRole('textbox', { name: 'Program name for Drum Bank' }) as HTMLInputElement;
        expect(name.maxLength).toBe(8);
        await fireEvent.input(name, { target: { value: 'Drums' } });
        expect(onnamechange).toHaveBeenCalledWith('bank-1', 'Drums');

        await fireEvent.click(screen.getByRole('checkbox', { name: 'Generate Program for Kick' }));
        expect(onselectionchange).toHaveBeenCalledWith('sample-1', true);
        const selectAll = screen.getByRole('checkbox', { name: 'Select all targets' }) as HTMLInputElement;
        expect(selectAll.checked).toBe(true);
        expect(selectAll.indeterminate).toBe(false);
        await fireEvent.click(selectAll);
        expect(onselectall).toHaveBeenCalledWith(false);
        await fireEvent.click(screen.getByRole('button', { name: 'Generate 1 Program' }));
        expect(onconfirm).toHaveBeenCalledOnce();
    });

    it('uses the compact single-scroller layout established by batch package import', () => {
        render(ProgramGenerationDialog, {
            props: {
                volumeName: 'Drums',
                inspection,
                rows,
                loading: false,
                busy: false,
                error: '',
                onselectionchange: vi.fn(),
                onnamechange: vi.fn(),
                onselectall: vi.fn(),
                onconfirm: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        const dialog = screen.getByRole('dialog', { name: 'Generate Programs' });
        expect(dialog.classList.contains('dialog-shell-wide')).toBe(true);
        const content = dialog.querySelector('.program-generation-content');
        const footer = screen.getByText('1 selected').closest('footer');
        expect(content).toBeTruthy();
        expect(footer?.classList.contains('dialog-footer')).toBe(true);
        expect(content?.contains(footer)).toBe(false);

        expect(dialogSource).toMatch(
            /\.program-generation-dialog\s*\{[^}]*width:\s*min\(1080px, calc\(100vw - 32px\)\);[^}]*max-height:\s*min\(860px, calc\(100vh - 32px\)\);/s,
        );
        expect(dialogSource).toMatch(
            /\.program-generation-content\s*\{[^}]*grid-auto-rows:\s*max-content;[^}]*overflow:\s*auto;/s,
        );
        expect(dialogSource).toMatch(/\.program-generation-row\s*\{[^}]*align-items:\s*start;/s);
        expect(dialogSource).toMatch(/\.program-generation-row\s*\{[^}]*padding-block:\s*5px;/s);
        expect(dialogSource).toMatch(/\.excluded\s*>\s*:not\(\.selection-cell\)\s*\{[^}]*opacity:\s*0\.55;/s);
        expect(dialogSource).not.toMatch(/\.program-generation-rows\s*\{[^}]*(?:max-height|overflow-y):/s);
        expect(dialogSource).toMatch(/@media \(max-width:\s*760px\)/);
    });
});
