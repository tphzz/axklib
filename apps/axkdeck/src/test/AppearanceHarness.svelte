<script lang="ts">
    import StartupShell from '../StartupShell.svelte';
    import WorkspaceShell from '../features/workspace/WorkspaceShell.svelte';
    import type { WorkspaceMode } from '../features/workspace/contracts';
    import ImageActions from '../lib/components/ImageActions.svelte';
    import AboutDialog from '../lib/components/AboutDialog.svelte';
    import Icon from '../lib/components/Icon.svelte';
    import Tx16wImportDialog from '../lib/components/Tx16wImportDialog.svelte';
    import type { Tx16wImportRequest } from '../features/import/tx16wWorkflow.svelte';
    import { ImportCompletion } from '../features/import/importCompletion.svelte';
    import { JobController } from '../features/jobs/actions';
    import '../features/object-editor/editor.css';

    const params = new URLSearchParams(location.search);
    const stage = params.get('stage');
    const noop = () => {};
    const states = ['Default', 'Hover', 'Focus', 'Disabled', 'Busy'];
    let mode = $state<WorkspaceMode>('device');
    let about = $state(false);
    const unexpectedJob = async () => {
        throw new Error('Appearance fixtures do not submit jobs');
    };
    const completion = new ImportCompletion(
        { waitForJob: unexpectedJob },
        new JobController({ waitForJob: unexpectedJob, cancelJob: unexpectedJob }),
    );
    const tx16wRequest: Tx16wImportRequest = {
        members: [],
        target: null,
        importMode: 'HIERARCHY',
        plan: null,
        fidelity: 'BEST',
        acknowledgedIssueIds: [],
        status: stage === 'tx16w-busy' ? 'inspecting' : 'ready',
        progress: 0,
        error: '',
    };
</script>

{#snippet imageActions()}
    <ImageActions
        image={null}
        opening={false}
        storageLocationsAvailable={true}
        onopen={noop}
        oncreate={noop}
        onclose={noop}
        onmanagelocations={noop}
    />
{/snippet}
{#snippet navigation()}<div class="fixture-navigation">Partitions and volumes</div>{/snippet}
{#snippet inspector()}<aside class="fixture-inspector">Object details</aside>{/snippet}
{#snippet content()}
    <section class="appearance-content" aria-label="Action states">
        {#each states as state}
            <div class="appearance-row" data-action-state={state}>
                <span>{state}</span>
                <button class="primary-button" disabled={state === 'Disabled'} aria-busy={state === 'Busy'}>
                    <Icon name="file-plus" size={13} />Create
                </button>
                <button class="secondary-button" disabled={state === 'Disabled'} aria-busy={state === 'Busy'}
                    >Cancel</button
                >
            </div>
        {/each}
        <div class="dialog-segmented-control" role="group" aria-label="Blue access choice">
            <button aria-pressed="true">Read only</button><button aria-pressed="false">Read &amp; write</button>
        </div>
        <div class="editor-choices" role="group" aria-label="Editor value choice">
            <button class="editor-choice" aria-pressed="true">On</button>
            <button class="editor-choice" aria-pressed="false">Off</button>
        </div>
        <footer class="dialog-footer">
            <span class="dialog-footer-status">Ready</span>
            <div class="dialog-footer-actions">
                <button class="secondary-button">Connection settings</button>
                <button class="primary-button">Create image</button>
                <button class="danger-button">Erase</button>
            </div>
        </footer>
    </section>
{/snippet}

{#if stage === 'starting' || stage === 'unavailable'}
    <StartupShell
        status={stage}
        warningOpen={false}
        message="Fixture unavailable"
        onacknowledge={noop}
        onretry={noop}
        onopensettings={noop}
    />
{:else}
    <WorkspaceShell
        {mode}
        presentation={{ navigation, content, inspector }}
        {imageActions}
        imageName="Fixture source"
        context="Read-only fixture"
        status="Ready"
        inspectorOpen={!params.has('narrow')}
        onmodechange={(value) => (mode = value)}
        onabout={() => (about = true)}
    />
{/if}
{#if stage?.startsWith('tx16w-')}
    <Tx16wImportDialog
        request={tx16wRequest}
        volumeOptions={[]}
        {completion}
        onrecover={noop}
        ontarget={noop}
        onmode={noop}
        onfidelity={noop}
        onprepare={noop}
        onadd={noop}
        onremove={noop}
        onconfirm={noop}
        oncancel={noop}
    />
{/if}
{#if about}
    <AboutDialog
        state={{
            status: 'ready',
            buildInfo: {
                schemaVersion: 1,
                semanticVersion: '0.0.0',
                projectVersion: '0.0.0',
                sourceIdentity: 'fixture',
                releaseTag: '',
                isRelease: false,
                webviewEngine: 'Fixture',
                webviewVersion: null,
            },
        }}
        onclose={() => (about = false)}
    />
{/if}

<style>
    .fixture-navigation,
    .fixture-inspector {
        padding: 12px 8px;
        color: var(--color-text);
        font-size: 11px;
    }
    .appearance-content {
        min-width: 0;
        min-height: 0;
        overflow: auto;
        padding: 12px 8px;
    }
    .appearance-row {
        display: grid;
        grid-template-columns: 60px 112px 112px;
        align-items: center;
        gap: 8px;
        margin-bottom: 12px;
        color: var(--color-text);
        font-size: 11px;
    }
    .dialog-footer {
        padding: 12px 0;
    }
    .dialog-footer-actions {
        flex-wrap: wrap;
    }
</style>
