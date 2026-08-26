<script lang="ts">
    import ServerStoragePicker from '../../lib/components/ServerStoragePicker.svelte';
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
    <ServerStoragePicker
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
            finish(null);
            manageLocations();
        }}
        onselect={(selection) => finish(selection)}
        onselectmany={(selections) => finish(selections)}
        oncancel={() => finish(null)}
    />
{/if}
