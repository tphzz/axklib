import { sampleFormatFixture } from '../../test/sampleFormatFixture';
/// <reference types="node" />

import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import type { ObjectDetail } from '../../lib/transport';
import SampleDuplicateDialog from './SampleDuplicateDialog.svelte';
import { ObjectEditorWorkflow } from './workflow.svelte';

const appStyles = readFileSync(resolve(process.cwd(), 'src/app.css'), 'utf8');

function detail(): ObjectDetail {
    return {
        image: { revision: 1 },
        object: { id: 'source', key: 'source', name: 'Source' },
        editing: {
            profile: 'a-series/sample',
            editable: true,
            reason: '',
            payloadSha256: 'a'.repeat(64),
            parameters: { level: 100, pan: 0, loop_mode: 4, loop_start_frame: 0, loop_length_frames: 100 },
            playbackWindow: { start_frame: 0, length_frames: 100 },
            maximumFrames: 100,
            canEditPlayback: true,
            eqCoefficients: [-15904, 7738, 8192, 15904, -7738],
            blockedParameters: [],
            blockedParameterReasons: {},
            ...sampleFormatFixture(),
            unavailableParameters: {},
            partitionIndex: 0,
            volumeName: 'Volume',
            sources: [],
        },
    } as unknown as ObjectDetail;
}

function setup() {
    let resolveSource!: (value: ObjectDetail) => void;
    const pendingSource = new Promise<ObjectDetail>((resolve) => (resolveSource = resolve));
    const transport = {
        objectDetail: vi.fn().mockReturnValueOnce(pendingSource).mockResolvedValue(detail()),
        startObjectParameterEdit: vi.fn(),
        startSampleDuplication: vi.fn().mockResolvedValue({ jobId: 7, status: 'queued' }),
        waitForJob: vi.fn().mockResolvedValue({ jobId: 7, status: 'completed' }),
    };
    const editor = new ObjectEditorWorkflow({
        transport,
        refresh: vi.fn().mockResolvedValue(undefined),
        stopPlayback: vi.fn(),
        status: vi.fn(),
    });
    const workflow = editor.duplication;
    const oncreated = vi.fn(async (_: string) => undefined);
    const opening = workflow.open(1, 'source', ['Source'], oncreated);
    const component = render(SampleDuplicateDialog, { props: { workflow } });
    return { workflow, transport, oncreated, opening, resolveSource, ...component };
}

describe('SampleDuplicateDialog', () => {
    it('focuses and selects the suggested name when delayed loading enables the input', async () => {
        const { opening, resolveSource } = setup();
        const input = screen.getByRole('textbox', { name: 'Sample name' }) as HTMLInputElement;
        expect(input.disabled).toBe(true);
        expect(screen.getByRole('status').textContent).toContain('Loading');
        await Promise.resolve();
        expect(document.activeElement).not.toBe(input);

        resolveSource(detail());
        await opening;

        await waitFor(() => expect(document.activeElement).toBe(input));
        expect(input.disabled).toBe(false);
        expect(input.value).toBe('Source Copy');
        expect(input.selectionStart).toBe(0);
        expect(input.selectionEnd).toBe(input.value.length);
    });

    it.each(['pointer', 'keyboard'] as const)(
        'does not steal focus after deliberate %s interaction during loading',
        async (interaction) => {
            const { opening, resolveSource } = setup();
            const input = screen.getByRole('textbox', { name: 'Sample name' }) as HTMLInputElement;
            const cancel = screen.getByRole('button', { name: 'Cancel' });
            await Promise.resolve();
            if (interaction === 'pointer') await fireEvent.pointerDown(cancel);
            else await fireEvent.keyDown(screen.getByRole('button', { name: 'Close' }), { key: 'Tab' });
            cancel.focus();
            expect(document.activeElement).toBe(cancel);

            resolveSource(detail());
            await opening;

            await waitFor(() => expect(input.disabled).toBe(false));
            expect(input.value).toBe('Source Copy');
            expect(document.activeElement).toBe(cancel);
        },
    );

    it('uses shared compact input and stable mixed-action footer geometry', async () => {
        const actionGeometry = appStyles.match(
            /\.secondary-button,\s*\.primary-button,\s*\.danger-button\s*\{[^}]+\}/,
        )?.[0];
        const dialogActionGeometry = appStyles.match(
            /\.dialog-footer \.secondary-button,\s*\.dialog-footer \.primary-button,\s*\.dialog-footer \.danger-button\s*\{[^}]+\}/,
        )?.[0];
        expect(actionGeometry).toBeDefined();
        expect(dialogActionGeometry).toBeDefined();
        const style = document.createElement('style');
        style.textContent = `${actionGeometry}\n${dialogActionGeometry}`;
        document.head.append(style);
        try {
            const { opening, resolveSource } = setup();
            const input = screen.getByRole('textbox', { name: 'Sample name' }) as HTMLInputElement;
            const cancel = screen.getByRole('button', { name: 'Cancel' });
            const duplicate = screen.getByRole('button', { name: 'Duplicate' });
            const status = screen.getByRole('status');
            expect(input.classList.contains('dialog-field-control')).toBe(true);
            expect(input.maxLength).toBe(16);
            expect(input.autocomplete).toBe('off');
            expect(cancel.closest('.dialog-footer-actions')).toBe(duplicate.closest('.dialog-footer-actions'));
            expect(duplicate.closest('.dialog-footer-actions')).not.toBeNull();
            expect(status.classList.contains('dialog-footer-status')).toBe(true);
            expect(status.closest('.dialog-footer')).toBe(duplicate.closest('.dialog-footer'));
            expect(duplicate.closest('.dialog-footer')).not.toBeNull();
            const cancelStyle = getComputedStyle(cancel);
            const duplicateStyle = getComputedStyle(duplicate);
            expect(cancelStyle.height).toBe('30px');
            expect(duplicateStyle.height).toBe('30px');
            for (const computed of [cancelStyle, duplicateStyle]) {
                expect(computed.marginTop).toBe('0px');
                expect(computed.marginBottom).toBe('0px');
            }
            resolveSource(detail());
            await opening;
            await waitFor(() => expect(input.disabled).toBe(false));
            expect(screen.getByRole('status')).toBe(status);
            expect(screen.getByRole('button', { name: 'Duplicate' })).toBe(duplicate);
        } finally {
            style.remove();
        }
    });

    it('uses the shared Escape path to cancel loading and ignores its late source result', async () => {
        const { workflow, opening, resolveSource, transport } = setup();
        await fireEvent.keyDown(screen.getByRole('dialog', { name: 'Duplicate Sample' }), { key: 'Escape' });
        expect(workflow.visible).toBe(false);
        resolveSource(detail());
        await opening;
        expect(workflow.visible).toBe(false);
        expect(workflow.source).toBeNull();
        expect(transport.startSampleDuplication).not.toHaveBeenCalled();
    });
});
