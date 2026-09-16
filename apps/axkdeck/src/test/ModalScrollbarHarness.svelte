<script lang="ts">
    import AboutDialog from '../lib/components/AboutDialog.svelte';
    import ServerStoragePicker from '../lib/components/ServerStoragePicker.svelte';
    import { storagePickerDirectory, storagePickerFixture } from './storagePickerFixture';

    let dialog = $state<'picker' | 'about' | null>(null);
    const folderMode = new URLSearchParams(window.location.search).has('folders');
    const transport = storagePickerFixture(true);
    if (folderMode) {
        const list = transport.sandboxDirectory.bind(transport);
        transport.sandboxDirectory = async (...args) => {
            const result = await list(...args);
            return { ...result, entries: result.entries.map((entry) => ({ ...entry, kind: 'DIRECTORY', size: null })) };
        };
    }
</script>

<main class="scrollbar-workspace">
    <header>
        <button class="secondary-button" onclick={() => (dialog = 'picker')}>Import floppy</button>
        <button class="secondary-button" onclick={() => (dialog = 'about')}>About</button>
    </header>
    {#each ['Navigation', 'Center', 'Inspector', 'Lower tools'] as pane}
        <section class="background-pane" aria-label={pane}>
            {#each Array.from({ length: 100 }, (_, index) => index) as row}
                <p>{pane} row {row + 1}</p>
            {/each}
        </section>
    {/each}
</main>

{#if dialog === 'picker'}
    <ServerStoragePicker
        {transport}
        title="Choose floppy images"
        mode={folderMode ? 'save-file' : 'file'}
        multiple={!folderMode}
        extensions={['hds']}
        initialDirectory={storagePickerDirectory}
        onselect={() => (dialog = null)}
        oncancel={() => (dialog = null)}
    />
{:else if dialog === 'about'}
    <AboutDialog state={{ status: 'loading' }} onclose={() => (dialog = null)} />
{/if}

<style>
    .scrollbar-workspace {
        display: grid;
        height: 100%;
        grid-template-columns: 40% 35% 25%;
        grid-template-rows: 48px minmax(0, 3fr) minmax(0, 1fr);
    }

    header {
        grid-column: 1 / -1;
    }

    .background-pane {
        min-width: 0;
        min-height: 0;
        overflow: auto;
        scrollbar-gutter: stable;
        border: 1px solid var(--color-border);
    }

    .background-pane:last-child {
        grid-column: 1 / -1;
    }

    p {
        min-width: 450px;
        height: 32px;
        margin: 0;
    }
</style>
