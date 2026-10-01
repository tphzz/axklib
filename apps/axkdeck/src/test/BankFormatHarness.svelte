<script lang="ts">
    import type { ImageTransport, ObjectDetail } from '../lib/transport';
    import type { SampleStructureItem } from '../lib/types';
    import type { ObjectFormatConversionRequest, SampleStorageFormat } from '../lib/objectEditing';
    import { ObjectEditorWorkflow } from '../features/object-editor/workflow.svelte';
    import SampleFormatDialog from '../features/object-editor/SampleFormatDialog.svelte';
    import SampleFormatDetails from '../features/object-editor/SampleFormatDetails.svelte';
    import SampleCollectionHarness from './SampleCollectionHarness.svelte';
    import { sampleConversionFixture, sampleFormatFixture } from './sampleFormatFixture';
    let format = $state<SampleStorageFormat>(
        new URLSearchParams(location.search).has('native') ? 'A3000_188' : 'A4000_A5000_224',
    );
    let writes = $state(0);
    let status = $state('');
    let failRefresh = true;
    const blocked = new URLSearchParams(location.search).has('pending');
    function item(id: string, stored: SampleStorageFormat, bank = false): SampleStructureItem {
        const objectType = bank ? 'SBAC' : 'SBNK';
        return {
            id,
            objectId: id,
            name: id,
            objectType,
            membershipLabel: '',
            object: {
                key: id,
                name: id,
                objectType,
                partitionIndex: 0,
                partitionName: 'Partition',
                volumeName: 'Volume',
                categoryName: objectType,
                objectEncoding: 'current',
                directoryEntryName: id,
                sfsId: 1,
                storedSizeBytes: 528,
                sizeWithDependenciesBytes: 2048,
                sampleRate: 44100,
                rootKey: 60,
                storedFrameCount: 0,
                waveStartFrame: 0,
                waveLengthFrames: 0,
                sampleWidthBytes: 2,
                storageState: 'COMPLETE',
                sampleFormat: sampleFormatFixture(stored).sampleFormat,
            },
        };
    }
    const members = [item('Native member', 'A3000_188'), item('Later member', 'A4000_A5000_224')];
    function detail(): ObjectDetail {
        const capability = sampleConversionFixture(format, { payloadSha256: String(writes + 1).repeat(64) });
        if (blocked) {
            capability.formatConversions[0]!.allowed = false;
            capability.formatConversions[0]!.blockers = [
                {
                    key: 'active_overrides',
                    storedValue: null,
                    message: 'Active bank overrides cannot be converted or discarded.',
                },
            ];
        }
        return {
            image: { revision: writes + 1 },
            object: { id: 'Bank', key: 'Bank', name: 'Bank', type: 'SBAC' },
            editing: null,
            formatConversion: capability,
        } as unknown as ObjectDetail;
    }
    const transport = {
        objectDetail: async () => detail(),
        startObjectParameterEdit: async () => {
            throw new Error('Unexpected parameter edit');
        },
        startObjectFormatConversion: async (_: number, request: ObjectFormatConversionRequest) => {
            if (request.operation.type !== 'convert_sbac_format' || blocked) throw new Error('Unexpected conversion');
            format = request.operation.target_format === 'a3000_188' ? 'A3000_188' : 'A4000_A5000_224';
            writes++;
            return { jobId: writes, status: 'queued' };
        },
        waitForJob: async (jobId: number) => ({ jobId, status: 'completed' }),
    } as unknown as ImageTransport;
    const workflow = new ObjectEditorWorkflow({
        transport,
        stopPlayback: () => {},
        status: (text) => (status = text),
        refresh: async () => {
            if (failRefresh) {
                failRefresh = false;
                throw new Error('Test refresh interruption');
            }
        },
    });
</script>

<main>
    <SampleCollectionHarness
        {workflow}
        samples={members}
        sampleBanks={[item('Bank', format, true)]}
        view="sample-banks"
        initiallySelected={['Bank']}
        initiallyActive=""
    />
    <aside><SampleFormatDetails format={sampleFormatFixture(format).sampleFormat} bank {members} /></aside>
    <output aria-label="Conversion writes">{writes}</output>
    <output aria-label="Workspace status">{status}</output>
</main>
{#if workflow.conversionDocument}<SampleFormatDialog {workflow} document={workflow.conversionDocument} />{/if}

<style>
    main {
        padding: 12px;
    }
    aside {
        max-width: 320px;
        padding: 12px;
    }
</style>
