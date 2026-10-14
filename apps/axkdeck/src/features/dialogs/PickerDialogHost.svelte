<script lang="ts">
    import ServerStoragePicker from '../../lib/components/ServerStoragePicker.svelte';
    import OpenSourceDialog from './OpenSourceDialog.svelte';
    import type { ImageTransport } from '../../lib/transport';
    import type { PickerRequest, PickerSelection } from './picker';

    interface Props {
        transport: ImageTransport;
        request: PickerRequest | null;
        finish: (selection: PickerSelection | null) => void;
        manageLocations: () => void;
    }

    let { transport, request, finish, manageLocations }: Props = $props();
</script>

{#if request}
    {#key request}
        {@const current = request}
        {@const Picker = current.purpose === 'open-source' ? OpenSourceDialog : ServerStoragePicker}
        <Picker
            {transport}
            mode={request.mode}
            title={request.title}
            extensions={request.extensions}
            suggestedName={request.suggestedName}
            multiple={request.multiple}
            initialDirectory={request.initialDirectory}
            initialFile={request.initialFile}
            requireWritableDirectory={request.requireWritableDirectory}
            ondirectorychange={request.ondirectorychange}
            onmanagelocations={() => {
                if (request !== current) return;
                finish(null);
                manageLocations();
            }}
            onselect={(selection) => {
                if (request === current) finish(selection);
            }}
            onselectmany={(selections) => {
                if (request === current) finish(selections);
            }}
            onselectsources={(selections) => {
                if (request === current) finish(selections);
            }}
            oncancel={() => {
                if (request === current) finish(null);
            }}
        />
    {/key}
{/if}
