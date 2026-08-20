<script lang="ts">
    import { audioExtensions } from '../import/audioWorkflow.svelte';
    import { midiExtensions } from '../../lib/midiImport';

    interface Props {
        audioInput?: HTMLInputElement;
        sequenceInput?: HTMLInputElement;
        audioChanged: (event: Event) => void;
        audioCancelled: () => void;
        sequenceChanged: (event: Event) => void;
        sequenceCancelled: () => void;
    }

    let {
        audioInput = $bindable(),
        sequenceInput = $bindable(),
        audioChanged,
        audioCancelled,
        sequenceChanged,
        sequenceCancelled,
    }: Props = $props();
</script>

<input
    bind:this={audioInput}
    class="sr-only"
    type="file"
    multiple
    accept={audioExtensions.map((extension) => `.${extension}`).join(',')}
    onchange={audioChanged}
    oncancel={audioCancelled}
/>
<input
    bind:this={sequenceInput}
    class="sr-only"
    type="file"
    multiple
    accept={midiExtensions.map((extension) => `.${extension}`).join(',')}
    onchange={sequenceChanged}
    oncancel={sequenceCancelled}
/>
