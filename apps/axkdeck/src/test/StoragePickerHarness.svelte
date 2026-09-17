<script lang="ts">
    import ServerStoragePicker from '../lib/components/ServerStoragePicker.svelte';
    import { storagePickerDirectory, storagePickerFixture } from './storagePickerFixture';

    const transport = storagePickerFixture(new URLSearchParams(window.location.search).has('long'));
</script>

<ServerStoragePicker
    {transport}
    title="Open image"
    mode="file"
    multiple={true}
    extensions={['hds']}
    initialDirectory={storagePickerDirectory}
    onselect={() => {}}
    oncancel={() => {}}
/>
