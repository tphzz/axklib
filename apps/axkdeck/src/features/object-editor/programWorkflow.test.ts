import { describe, expect, it, vi } from 'vitest';
import type { ProgramEditingSnapshot, ProgramEditorCatalog } from '../../lib/objectEditing';
import type { JobState, ObjectDetail } from '../../lib/transport';
import { programConversionFixture } from '../../test/programFormatFixture';
import { ObjectEditorWorkflow } from './workflow.svelte';
import { ProgramDraft } from '../devices/a-series/program/draft.svelte';
import { programEditorFixture } from '../../test/programEditorFixture';

const catalog: ProgramEditorCatalog = {
    schemaVersion: 1,
    formats: [{ model: 'A5000', fields: [{ key: 'level', min: 0, max: 127 }], effects: [] }],
};

function detail(revision = 1, payloadSha256 = 'a'.repeat(64)): ObjectDetail {
    const editing: ProgramEditingSnapshot = {
        profile: 'a-series/program',
        editable: true,
        reason: '',
        payloadSha256,
        partitionIndex: 0,
        volumeName: 'Volume',
        programNumber: 33,
        programName: 'Test',
        storageRevision: 4,
        model: 'A5000',
        assignments: [],
        targets: [],
        values: { level: 100 },
    };
    return {
        image: { revision },
        object: { id: 'program', key: 'program', name: '033', type: 'PROG' },
        editing,
        formatConversion: { ...programConversionFixture(true), payloadSha256 },
    } as unknown as ObjectDetail;
}

function setup() {
    const transport = {
        objectDetail: vi.fn().mockResolvedValue(detail()),
        programEditorCatalog: vi.fn<() => Promise<ProgramEditorCatalog>>().mockResolvedValue(catalog),
        startObjectParameterEdit: vi.fn().mockResolvedValue({ jobId: 7, status: 'queued' }),
        startObjectFormatConversion: vi.fn(),
        waitForJob: vi
            .fn<() => Promise<JobState>>()
            .mockResolvedValue({ jobId: 7, kind: 'alteration', status: 'completed' }),
    };
    const refresh = vi.fn().mockResolvedValue(undefined);
    const workflow = new ObjectEditorWorkflow({ transport, refresh, stopPlayback: vi.fn(), status: vi.fn() });
    return { workflow, transport, refresh };
}

describe('Program editor lifecycle', () => {
    it('discard restores the selected retained ordinal rather than its shortened draft index', async () => {
        const fixture = programEditorFixture();
        const { workflow, transport } = setup();
        transport.objectDetail.mockResolvedValue(fixture.document.detail!);
        transport.programEditorCatalog.mockResolvedValue(fixture.catalog);
        const document = (await workflow.load(1, 'program'))!;
        document.programAssignmentId = 1;
        (document.draft as ProgramDraft).removeAssignment(0);
        document.inputErrors['assignments.1.level_offset'] = 'Invalid offset';
        await workflow.discard(document);
        expect(document.programAssignmentId).toBe(1);
        expect((document.draft as ProgramDraft).assignments).toHaveLength(2);
        expect(document.inputErrors).toEqual({});
        expect(document.draft.dirty).toBe(false);
    });

    it('saves membership and parameters together and rebinds retained rows to reopened ordinals', async () => {
        const fixture = programEditorFixture();
        const { workflow, transport } = setup();
        const original = fixture.document.detail!;
        transport.objectDetail.mockResolvedValue(original);
        transport.programEditorCatalog.mockResolvedValue(fixture.catalog);
        const document = (await workflow.load(1, 'program'))!;
        document.programAssignmentId = 1;
        (document.draft as ProgramDraft).removeAssignment(0);
        document.draft.patch({ level: 80, 'assignments.1.level_offset': 55 });
        const saved = structuredClone(original);
        saved.image.revision = 2;
        const editing = saved.editing as ProgramEditingSnapshot;
        editing.payloadSha256 = 'b'.repeat(64);
        editing.assignments = [{ ...editing.assignments[1]!, ordinal: 0 }];
        editing.values = Object.fromEntries(
            Object.entries(editing.values)
                .filter(([key]) => !key.startsWith('assignments.0.'))
                .map(([key, value]) => [key.replace(/^assignments\.1\./, 'assignments.0.'), value]),
        );
        Object.assign(editing.values, { level: 80, 'assignments.0.level_offset': 55 });
        transport.objectDetail.mockResolvedValueOnce(original).mockResolvedValue(saved);
        await workflow.save(document);
        expect(transport.startObjectParameterEdit).toHaveBeenLastCalledWith(
            1,
            expect.objectContaining({
                operation: expect.objectContaining({
                    type: 'replace_program_assignments',
                    parameters: { level: 80 },
                    assignments: [{ retain_ordinal: 1, sample: 'Duplicate', parameters: { level_offset: 55 } }],
                }),
            }),
        );
        expect(document.draft.dirty).toBe(false);
        expect(document.programAssignmentId).toBe(0);
        document.draft.set('assignments.0.pan_offset', 5);
        await workflow.save(document);
        expect(transport.startObjectParameterEdit).toHaveBeenLastCalledWith(
            1,
            expect.objectContaining({
                expectedRevision: 2,
                operation: expect.objectContaining({
                    type: 'update_program_parameters',
                    expected_payload_sha256: 'b'.repeat(64),
                    assignments: [
                        {
                            ordinal: 0,
                            expected_target_kind: 'SBNK',
                            expected_target_name: 'Duplicate',
                            parameters: { pan_offset: 5 },
                        },
                    ],
                }),
            }),
        );
    });

    it('waits for its typed catalog before publishing the editable document', async () => {
        const { workflow, transport } = setup();
        let resolve!: (value: ProgramEditorCatalog) => void;
        transport.programEditorCatalog.mockReturnValueOnce(new Promise((complete) => (resolve = complete)));
        const pending = workflow.load(1, 'program');
        await vi.waitFor(() => expect(transport.programEditorCatalog).toHaveBeenCalledTimes(1));
        expect(workflow.documents).toEqual([]);
        resolve(catalog);
        const document = (await pending)!;
        expect(document.programFormat).toEqual(catalog.formats[0]);
        expect(document.draft.values).toEqual({ level: 100 });
        document.draft.set('level', 80);
        expect(document.canSave).toBe(true);
        expect(await workflow.load(1, 'program')).toBe(document);
        expect(transport.programEditorCatalog).toHaveBeenCalledTimes(1);
    });

    it('does not cache a failed catalog request or publish a partially loaded editor', async () => {
        const { workflow, transport } = setup();
        transport.programEditorCatalog.mockRejectedValueOnce(new Error('Catalog unavailable'));
        await expect(workflow.load(1, 'program')).rejects.toThrow('Catalog unavailable');
        expect(workflow.documents).toEqual([]);
        const document = (await workflow.load(1, 'program'))!;
        expect(document.programFormat).toEqual(catalog.formats[0]);
        expect(transport.programEditorCatalog).toHaveBeenCalledTimes(2);
    });

    it.each([undefined, { schemaVersion: 1 as const, formats: [] }])(
        'rejects a missing Program format instead of publishing a permanently loading editor',
        async (missing) => {
            const { workflow, transport } = setup();
            transport.programEditorCatalog.mockResolvedValueOnce(missing as ProgramEditorCatalog);
            await expect(workflow.load(1, 'program')).rejects.toThrow('Program parameter catalog is unavailable');
            expect(workflow.documents).toEqual([]);
            expect((await workflow.load(1, 'program'))?.programFormat).toEqual(catalog.formats[0]);
        },
    );

    it('refuses a stale Program payload without losing the draft or starting a write', async () => {
        const { workflow, transport } = setup();
        const document = (await workflow.load(1, 'program'))!;
        document.draft.set('level', 80);
        transport.objectDetail.mockResolvedValue(detail(2, 'b'.repeat(64)));
        await workflow.save(document);
        expect(document.conflict).toContain('changed outside');
        expect(document.draft.values.level).toBe(80);
        expect(document.draft.dirty).toBe(true);
        expect(transport.startObjectParameterEdit).not.toHaveBeenCalled();
    });

    it('saves the numeric Program slot with the checked revision and exact payload guard', async () => {
        const { workflow, transport } = setup();
        const document = (await workflow.load(1, 'program'))!;
        document.draft.set('level', 80);
        transport.objectDetail.mockResolvedValue(detail(2));
        await workflow.save(document);
        expect(transport.startObjectParameterEdit).toHaveBeenCalledExactlyOnceWith(1, {
            expectedRevision: 2,
            operation: {
                id: 'program-edit',
                type: 'update_program_parameters',
                partition_index: 0,
                volume_name: 'Volume',
                program_number: 33,
                model: 'A5000',
                expected_payload_sha256: 'a'.repeat(64),
                parameters: { level: 80 },
                assignments: [],
            },
        });
        expect(document.draft.dirty).toBe(false);
        expect(document.phase).toBe('editable');
    });

    it('retains a failed save as dirty and refuses subsequent format conversion', async () => {
        const { workflow, transport, refresh } = setup();
        const document = (await workflow.load(1, 'program'))!;
        document.draft.set('level', 80);
        transport.waitForJob.mockResolvedValueOnce({
            jobId: 7,
            kind: 'alteration',
            status: 'failed',
            error: 'Write rejected',
        });
        await workflow.save(document);
        expect(document.status).toContain('Write rejected');
        expect(document.draft.dirty).toBe(true);
        expect(document.draft.values.level).toBe(80);
        expect(document.phase).toBe('editable');
        expect(refresh).not.toHaveBeenCalled();
        expect(workflow.conversionReason(document)).toContain('Save or discard');
        await workflow.convert(document, 'A3000');
        expect(transport.startObjectFormatConversion).not.toHaveBeenCalled();
    });

    it('saveAll waits for confirmed writing and workspace refresh before allowing continuation', async () => {
        const { workflow, transport, refresh } = setup();
        const document = (await workflow.load(1, 'program'))!;
        document.draft.set('level', 80);
        let completeWrite!: (job: JobState) => void;
        transport.waitForJob.mockReturnValueOnce(new Promise((resolve) => (completeWrite = resolve)));
        let completeRefresh!: () => void;
        refresh.mockReturnValueOnce(new Promise<void>((resolve) => (completeRefresh = resolve)));
        const continued = vi.fn();
        const pending = workflow.saveAll().then(continued);
        await vi.waitFor(() => expect(transport.waitForJob).toHaveBeenCalledOnce());
        expect(continued).not.toHaveBeenCalled();
        expect(refresh).not.toHaveBeenCalled();
        expect(workflow.locked).toBe(true);

        const saved = detail(2, 'b'.repeat(64));
        (saved.editing as ProgramEditingSnapshot).values.level = 80;
        transport.objectDetail.mockResolvedValue(saved);
        completeWrite({ jobId: 7, kind: 'alteration', status: 'completed' });
        await vi.waitFor(() => expect(refresh).toHaveBeenCalledOnce());
        expect(continued).not.toHaveBeenCalled();
        expect(document.draft.dirty).toBe(true);
        completeRefresh();
        await pending;
        expect(continued).toHaveBeenCalledExactlyOnceWith(true);
        expect(document.draft.values.level).toBe(80);
        expect(document.draft.dirty).toBe(false);
        expect(document.detail?.editing?.payloadSha256).toBe('b'.repeat(64));
        expect(workflow.locked).toBe(false);
    });

    it.each(['failed', 'cancelled', 'unconfirmed', 'refresh-failed'] as const)(
        'saveAll refuses continuation after a %s save and preserves all remaining drafts',
        async (outcome) => {
            const { workflow, transport, refresh } = setup();
            const document = (await workflow.load(1, 'program'))!;
            const other = detail();
            other.object = { ...other.object, id: 'other-program', key: 'other-program', name: '034' };
            (other.editing as ProgramEditingSnapshot).programNumber = 34;
            transport.objectDetail.mockResolvedValueOnce(other);
            const next = (await workflow.load(1, 'other-program'))!;
            document.draft.set('level', 80);
            next.draft.set('level', 70);
            if (outcome === 'refresh-failed') refresh.mockRejectedValueOnce(new Error('Workspace unavailable'));
            else if (outcome === 'unconfirmed') transport.waitForJob.mockRejectedValueOnce(new Error('Disconnected'));
            else
                transport.waitForJob.mockResolvedValueOnce({
                    jobId: 7,
                    kind: 'alteration',
                    status: outcome,
                    error: 'Write stopped',
                });

            expect(await workflow.saveAll()).toBe(false);
            expect(transport.startObjectParameterEdit).toHaveBeenCalledOnce();
            expect(document.phase).toBe(outcome === 'failed' || outcome === 'cancelled' ? 'editable' : outcome);
            expect(document.draft.dirty).toBe(true);
            expect(document.draft.values.level).toBe(80);
            expect(next.draft.dirty).toBe(true);
            expect(next.draft.values.level).toBe(70);
            if (outcome !== 'refresh-failed') expect(refresh).not.toHaveBeenCalled();
            if (outcome === 'unconfirmed' || outcome === 'refresh-failed') {
                expect(await workflow.saveAll()).toBe(false);
                expect(transport.startObjectParameterEdit).toHaveBeenCalledOnce();
                expect(document.jobId).toBe(7);
            }
        },
    );

    it('saveAll refuses invalid drafts without starting a write', async () => {
        const { workflow, transport } = setup();
        const document = (await workflow.load(1, 'program'))!;
        document.draft.set('level', 200);
        expect(await workflow.saveAll()).toBe(false);
        expect(document.draft.dirty).toBe(true);
        expect(transport.startObjectParameterEdit).not.toHaveBeenCalled();
    });
});
