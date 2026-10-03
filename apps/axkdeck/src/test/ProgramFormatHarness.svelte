<script lang="ts">
    import type { ImageTransport, ObjectDetail, SamplerObject, SystemProgramContexts } from '../lib/transport';
    import type { Program } from '../lib/types';
    import type { ObjectFormatConversionRequest, ProgramFormatMetadata } from '../lib/objectEditing';
    import ProgramWorkspace, { type ProgramPresentation } from '../lib/components/ProgramWorkspace.svelte';
    import ProgramFormatDetails from '../features/object-editor/ProgramFormatDetails.svelte';
    import ObjectFormatDialog from '../features/object-editor/ObjectFormatDialog.svelte';
    import { ObjectEditorWorkflow } from '../features/object-editor/workflow.svelte';
    import { programConversionFixture, programFormatFixture } from './programFormatFixture';

    const auditRecords =
        (window as Window & { programStorageAudit?: { name: string; format: ProgramFormatMetadata }[] })
            .programStorageAudit ?? [];
    let auditIndex = $state(0);
    const params = new URLSearchParams(location.search);
    const blocked = params.has('blocked');
    const readOnly = params.has('readonly');
    let current = $state(true);
    const activeFormat = $derived(auditRecords[auditIndex]?.format ?? programFormatFixture(current));
    let writes = $state(0);
    let status = $state('');
    let presentation = $state<ProgramPresentation>('single');
    let query = $state('');
    let failRefresh = true;
    const object = $derived({
        key: 'program-033',
        objectType: 'PROG',
        name: '033',
        partitionIndex: 0,
        partitionName: 'Partition 1',
        volumeName: 'Volume',
        categoryName: 'PROG',
        objectEncoding: 'current',
        directoryEntryName: '033',
        sfsId: 33,
        storedSizeBytes: activeFormat.logicalSize ?? 0,
        sizeWithDependenciesBytes: auditRecords.length ? null : 520192,
        sampleRate: 0,
        rootKey: 0,
        storedFrameCount: 0,
        waveStartFrame: 0,
        waveLengthFrames: 0,
        sampleWidthBytes: 0,
        storageState: 'COMPLETE',
        programFormat: activeFormat,
    } satisfies SamplerObject);
    const program = $derived({
        id: object.key,
        objectId: object.key,
        slot: '033',
        programNumber: 33,
        name: auditRecords[auditIndex]?.name ?? 'Ambient pads',
        object,
    } satisfies Program);
    const contexts: SystemProgramContexts = {
        partitionIndex: 0,
        message: '',
        files: [
            {
                fileKind: 'SYSTEM2',
                availability: 'AVAILABLE',
                storageRevision: 0,
                savedProgramMode: 'MULTI',
                basicReceive: { port: 'A', channel: 1, display: 'A01' },
                omni: false,
                programChangeEnabled: true,
                parts: [
                    {
                        partNumber: 1,
                        partLabel: 'A01',
                        midi: { port: 'A', channel: 1, display: 'A01' },
                        programNumber: 33,
                        master: true,
                    },
                    {
                        partNumber: 2,
                        partLabel: 'A02',
                        midi: { port: 'A', channel: 2, display: 'A02' },
                        programNumber: 34,
                        master: false,
                    },
                ],
            },
        ],
    };
    function detail(): ObjectDetail {
        const conversion = programConversionFixture(current);
        conversion.programName = program.name;
        conversion.payloadSha256 = String(writes + 1).repeat(64);
        conversion.canConvertFormat = !readOnly;
        conversion.reason = readOnly ? 'This image is read-only.' : '';
        conversion.formatConversions[0]!.changes = [
            current
                ? 'Store the Program in A3000 V2 format.'
                : 'Initialize the additional A4000/A5000 settings from the A3000 Program.',
            'Keep the Program slot, name, assignments and linked objects unchanged.',
        ];
        if (blocked) {
            conversion.formatConversions[0]!.allowed = false;
            conversion.formatConversions[0]!.blockers = Array.from({ length: 12 }, (_, i) => ({
                key: `effect.${i}`,
                storedValue: i,
                message: `Setting ${i + 1}: this Program uses an A4000/A5000 parameter that A3000 cannot retain.`,
            }));
        }
        return {
            image: { revision: writes + 1 },
            object: { id: program.objectId, key: object.key, type: 'PROG', name: '033' },
            editing: null,
            formatConversion: conversion,
        } as unknown as ObjectDetail;
    }
    const transport = {
        objectDetail: async () => detail(),
        startObjectParameterEdit: async () => {
            throw new Error('Unexpected parameter edit');
        },
        startObjectFormatConversion: async (_: number, request: ObjectFormatConversionRequest) => {
            if (
                request.operation.type !== 'convert_prog_format' ||
                request.operation.program_number !== 33 ||
                blocked ||
                readOnly
            )
                throw new Error('Unexpected conversion');
            current = request.operation.target_format === 'a4000_a5000';
            writes++;
            return { jobId: writes, status: 'queued' };
        },
        waitForJob: async (jobId: number) => ({ jobId, status: 'completed' }),
    } as unknown as ImageTransport;
    const workflow = new ObjectEditorWorkflow({
        transport,
        stopPlayback: () => {},
        status: (value) => (status = value),
        refresh: async () => {
            if (failRefresh) {
                failRefresh = false;
                throw new Error('Test refresh interruption');
            }
        },
    });
</script>

<main>
    {#if auditRecords.length}
        <label
            >Source Program
            <select aria-label="Source Program" bind:value={auditIndex}>
                {#each auditRecords as record, index}<option value={index}>{record.name}</option>{/each}
            </select>
        </label>
    {/if}
    <section>
        <ProgramWorkspace
            programs={[program]}
            {contexts}
            contextsLoading={false}
            contextsError=""
            {presentation}
            selectedPartNumber={null}
            activeObjectId={program.objectId}
            {query}
            onquerychange={(value) => (query = value)}
            onpresentationchange={(value) => (presentation = value)}
            onprogramselect={() => {}}
            onpartselect={() => {}}
            onconvertprogram={(selected) => void workflow.openConversion(1, selected.objectId)}
        />
    </section>
    <aside><ProgramFormatDetails format={activeFormat} /></aside>
    <output aria-label="Conversion writes">{writes}</output>
    <output aria-label="Workspace status">{status}</output>
</main>
{#if workflow.conversionDocument}<ObjectFormatDialog {workflow} document={workflow.conversionDocument} />{/if}

<style>
    main {
        padding: 12px;
    }
    section {
        height: 230px;
    }
    aside {
        max-width: 320px;
        padding: 12px;
    }
</style>
