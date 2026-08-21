<script lang="ts">
    import type { Tx16wImportRequest, Tx16wVolumeOption } from '../../features/import/tx16wWorkflow.svelte';
    import { browserUploadSource } from '../clientUploadSource';
    import type { Tx16wImportMode } from '../transport';
    import { modal } from '../modal';
    import Icon from './Icon.svelte';

    interface Props {
        request: Tx16wImportRequest;
        volumeOptions: Tx16wVolumeOption[];
        ontarget: (target: Tx16wVolumeOption['target']) => void;
        onmode: (mode: Tx16wImportMode) => void;
        onadd: (files: ReturnType<typeof browserUploadSource>[]) => void;
        onremove: (memberId: number) => void;
        onconfirm: () => void;
        oncancel: () => void;
    }

    let { request, volumeOptions, ontarget, onmode, onadd, onremove, onconfirm, oncancel }: Props = $props();
    let targetLabel = $state('');
    const busy = $derived(['uploading', 'inspecting', 'importing'].includes(request.status));
    const inspection = $derived(request.inspection);
    const targetSelectionValid = $derived(
        volumeOptions.some(
            (option) =>
                option.label === targetLabel &&
                option.target.partitionIndex === request.target?.partitionIndex &&
                option.target.volumeName === request.target?.volumeName,
        ),
    );
    const canConfirm = $derived(request.status === 'ready' && inspection?.valid === true && targetSelectionValid);
    const blockedCount = $derived(inspection?.notices.filter((notice) => notice.disposition === 'BLOCKED').length ?? 0);

    $effect(() => {
        const target = request.target;
        if (!target) return;
        const option = volumeOptions.find(
            (candidate) =>
                candidate.target.partitionIndex === target.partitionIndex &&
                candidate.target.volumeName === target.volumeName,
        );
        if (option) targetLabel = option.label;
    });

    function selectTarget(value: string): void {
        targetLabel = value;
        const option = volumeOptions.find((candidate) => candidate.label === value);
        if (option) ontarget(option.target);
    }

    function dispositionLabel(disposition: string): string {
        if (disposition === 'EXACT') return 'Exact';
        if (disposition === 'APPROXIMATED') return 'Approximated';
        if (disposition === 'DEFAULTED') return 'Defaulted';
        if (disposition === 'OMITTED') return 'Not imported';
        return 'Blocked';
    }

    function addFiles(input: HTMLInputElement): void {
        const files = Array.from(input.files ?? []).map(browserUploadSource);
        input.value = '';
        if (files.length > 0) onadd(files);
    }
</script>

<div class="dialog-backdrop dialog-backdrop-raised" role="presentation">
    <div
        class="dialog-shell dialog-shell-wide tx16w-import-dialog"
        role="dialog"
        aria-modal="true"
        aria-label="Import TX16W disk set"
        use:modal={{ onescape: oncancel }}
    >
        <header class="dialog-header">
            <div>
                <Icon name="disc" size={16} />
                <h2>Import TX16W disk set</h2>
            </div>
            <button class="icon-button" type="button" aria-label="Close" disabled={busy} onclick={oncancel}>
                <Icon name="close" size={15} />
            </button>
        </header>

        <div class="tx16w-content">
            <section class="disk-set" aria-label="Disk set">
                <div class="disk-set-heading">
                    <div>
                        <strong>Disk set</strong><small
                            >{request.members.length} image{request.members.length === 1 ? '' : 's'}</small
                        >
                    </div>
                    <label class="secondary-button add-disks">
                        <Icon name="file-plus" size={15} /> Add disks
                        <input
                            type="file"
                            accept=".img,.ima,application/octet-stream"
                            multiple
                            disabled={busy}
                            onchange={(event) => addFiles(event.currentTarget)}
                        />
                    </label>
                </div>
                <div class="disk-members">
                    {#each request.members as member (member.id)}
                        <div>
                            <Icon name="disc" size={14} />
                            <strong>{member.sourceName}</strong>
                            <button
                                class="icon-button"
                                type="button"
                                aria-label={`Remove ${member.sourceName}`}
                                title="Remove disk"
                                disabled={busy}
                                onclick={() => onremove(member.id)}><Icon name="trash" size={14} /></button
                            >
                        </div>
                    {/each}
                </div>
                {#if inspection}<small
                        >{inspection.profile === 'YAMAHA_NATIVE'
                            ? 'Yamaha native disk set'
                            : 'Yamaha native disk set with auxiliary files'} · {inspection.sourceMembers.length} recognized
                        {inspection.sourceMembers.length === 1 ? 'member' : 'members'}</small
                    >{/if}
            </section>

            <fieldset class="import-mode">
                <legend>Import content</legend>
                <label>
                    <input
                        type="radio"
                        name="tx16w-import-mode"
                        checked={request.importMode === 'HIERARCHY'}
                        disabled={busy}
                        onchange={() => onmode('HIERARCHY')}
                    />
                    <span><strong>Programs and samples</strong><small>Require a complete TX16W hierarchy</small></span>
                </label>
                <label>
                    <input
                        type="radio"
                        name="tx16w-import-mode"
                        checked={request.importMode === 'WAVE_DATA_ONLY'}
                        disabled={busy}
                        onchange={() => onmode('WAVE_DATA_ONLY')}
                    />
                    <span
                        ><strong>Wave Data only</strong><small
                            >Import recoverable wave payloads from an incomplete set</small
                        ></span
                    >
                </label>
            </fieldset>

            <label class="target-field">
                <span>Target volume</span>
                <input
                    class="dialog-field-control"
                    type="text"
                    list="tx16w-volume-options"
                    placeholder="Select a volume"
                    value={targetLabel}
                    disabled={busy || volumeOptions.length === 0}
                    autocomplete="off"
                    oninput={(event) => selectTarget(event.currentTarget.value)}
                />
                <datalist id="tx16w-volume-options">
                    {#each volumeOptions as option (option.key)}<option value={option.label}></option>{/each}
                </datalist>
            </label>

            {#if volumeOptions.length === 0}
                <p class="dialog-error" role="alert">The open image has no writable SFS volumes.</p>
            {:else if request.status === 'waiting-target'}
                <p class="tx16w-state">Select the SFS volume that should receive this disk set.</p>
            {:else if request.status === 'uploading'}
                <div class="tx16w-state" role="status">
                    <span>Uploading disk images…</span>
                    <progress max="1" value={request.progress}></progress>
                </div>
            {:else if request.status === 'inspecting'}
                <p class="tx16w-state" role="status">Reading TX16W Programs, Voices, Timbres, and Waves…</p>
            {/if}

            {#if inspection}
                <div class="object-counts" aria-label="Objects to import">
                    <span><strong>{inspection.counts.programs}</strong> Programs</span>
                    <span><strong>{inspection.counts.sampleBanks}</strong> Sample Banks</span>
                    <span><strong>{inspection.counts.samples}</strong> Samples</span>
                    <span><strong>{inspection.counts.waveData}</strong> Wave Data</span>
                </div>

                <div class="preview-sections">
                    <details open>
                        <summary>Programs <span>{inspection.objects.programs.length}</span></summary>
                        <div class="preview-table program-table">
                            {#each inspection.objects.programs as program (`${program.slot}:${program.name}`)}
                                <span>{String(program.slot).padStart(3, '0')}</span>
                                <strong>{program.name}</strong>
                                <span
                                    >{program.assignments.map((assignment) => assignment.name).join(', ') ||
                                        'No assignment'}</span
                                >
                            {/each}
                        </div>
                    </details>
                    <details>
                        <summary>Sample Banks <span>{inspection.objects.sampleBanks.length}</span></summary>
                        <div class="preview-table object-table">
                            {#each inspection.objects.sampleBanks as bank (bank.name)}
                                <strong>{bank.name}</strong><span>{bank.sampleNames.join(', ')}</span>
                            {/each}
                        </div>
                    </details>
                    <details>
                        <summary>Samples <span>{inspection.objects.samples.length}</span></summary>
                        <div class="preview-table sample-table">
                            {#each inspection.objects.samples as sample (sample.name)}
                                <strong>{sample.name}</strong><span>{sample.waveDataName}</span>
                                <span>Keys {sample.keyLow}–{sample.keyHigh} · root {sample.rootKey}</span>
                            {/each}
                        </div>
                    </details>
                    <details>
                        <summary>Wave Data <span>{inspection.objects.waveData.length}</span></summary>
                        <div class="preview-table object-table">
                            {#each inspection.objects.waveData as wave (wave.name)}
                                <strong>{wave.name}</strong><span>{wave.targetSampleRate.toLocaleString()} Hz</span>
                            {/each}
                        </div>
                    </details>
                </div>

                {#if inspection.notices.length > 0}
                    <details class:blocked={blockedCount > 0} class="mapping-notices" open={blockedCount > 0}>
                        <summary>
                            Parameter mapping <span
                                >{inspection.notices.length}
                                {inspection.notices.length === 1 ? 'notice' : 'notices'}</span
                            >
                        </summary>
                        <div class="notice-list">
                            {#each inspection.notices as notice (`${notice.disposition}:${notice.sourceObject}:${notice.sourceParameter}:${notice.targetObject}`)}
                                <div class:notice-blocked={notice.disposition === 'BLOCKED'} class="mapping-notice">
                                    <span class="disposition">{dispositionLabel(notice.disposition)}</span>
                                    <strong>{notice.sourceObject} · {notice.sourceParameter}</strong>
                                    <span>{notice.message}</span>
                                </div>
                            {/each}
                        </div>
                    </details>
                {/if}
            {/if}

            {#if request.error}<p class="dialog-error" role="alert">{request.error}</p>{/if}
        </div>

        <footer class="dialog-footer tx16w-footer">
            <span
                >{inspection
                    ? inspection.valid
                        ? 'Ready to import'
                        : `${blockedCount} blocking ${blockedCount === 1 ? 'issue' : 'issues'}`
                    : ''}</span
            >
            <div>
                <button class="secondary-button" type="button" disabled={busy} onclick={oncancel}>Cancel</button>
                <button class="primary-button" type="button" disabled={!canConfirm} onclick={onconfirm}>
                    {request.status === 'importing' ? 'Importing' : 'Import disk set'}
                </button>
            </div>
        </footer>
    </div>
</div>

<style>
    .tx16w-import-dialog {
        width: min(1120px, calc(100vw - 32px));
        max-height: min(880px, calc(100vh - 32px));
    }

    .tx16w-content {
        flex: 1 1 auto;
        min-height: 0;
        overflow: auto;
        display: grid;
        align-content: start;
        gap: 12px;
        padding: 14px 16px;
    }

    .disk-set-heading,
    .object-counts,
    .tx16w-footer,
    .tx16w-footer > div {
        display: flex;
        align-items: center;
    }

    .disk-set-heading {
        justify-content: space-between;
        gap: 16px;
    }

    .disk-set,
    .disk-set-heading > div,
    .target-field,
    .tx16w-state {
        display: grid;
        gap: 5px;
    }

    .disk-set {
        padding: 10px 12px;
        border: 1px solid var(--color-border);
        border-radius: 6px;
    }

    .disk-set-heading > div {
        grid-template-columns: auto auto;
        align-items: baseline;
        gap: 8px;
    }

    .add-disks {
        display: inline-flex;
        align-items: center;
        gap: 6px;
        cursor: pointer;
    }

    .add-disks input {
        display: none;
    }

    .disk-members {
        display: grid;
        grid-template-columns: repeat(auto-fit, minmax(230px, 1fr));
        gap: 4px 12px;
    }

    .disk-members > div {
        min-width: 0;
        display: grid;
        grid-template-columns: auto minmax(0, 1fr) auto;
        align-items: center;
        gap: 7px;
        padding: 3px 0;
    }

    .disk-members strong {
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
    }

    .import-mode {
        display: grid;
        grid-template-columns: 1fr 1fr;
        gap: 8px;
        padding: 8px 10px 10px;
        border: 1px solid var(--color-border);
        border-radius: 6px;
    }

    .import-mode legend {
        padding: 0 4px;
        color: var(--color-text-muted);
        font-size: 12px;
    }

    .import-mode label,
    .import-mode label > span {
        display: flex;
        align-items: flex-start;
        gap: 8px;
    }

    .import-mode label > span {
        flex-direction: column;
        gap: 2px;
    }

    small,
    .disk-set > small,
    summary span,
    .tx16w-footer > span {
        color: var(--color-text-muted);
        font-size: 12px;
    }

    .target-field > span {
        color: var(--color-text-muted);
        font-size: var(--dialog-label-font-size);
    }

    .target-field input {
        width: 100%;
    }

    .tx16w-state {
        color: var(--color-text-muted);
        margin: 2px 0;
    }

    progress {
        width: 100%;
    }

    .object-counts {
        gap: 24px;
        padding: 10px 12px;
        background: var(--color-panel-raised);
        border: 1px solid var(--color-border);
        border-radius: 6px;
    }

    .preview-sections {
        display: grid;
        gap: 6px;
    }

    details {
        border: 1px solid var(--color-border);
        border-radius: 5px;
        overflow: clip;
    }

    summary {
        cursor: pointer;
        display: flex;
        justify-content: space-between;
        gap: 12px;
        padding: 8px 10px;
        background: var(--color-panel-raised);
    }

    .preview-table {
        display: grid;
        gap: 0 14px;
        max-height: 210px;
        overflow: auto;
        padding: 4px 10px;
    }

    .preview-table > :global(*) {
        min-width: 0;
        padding: 6px 0;
        border-bottom: 1px solid rgb(61 68 72 / 65%);
        overflow-wrap: anywhere;
    }

    .program-table {
        grid-template-columns: 48px minmax(100px, 0.7fr) minmax(180px, 1.3fr);
    }

    .object-table {
        grid-template-columns: minmax(120px, 0.7fr) minmax(180px, 1.3fr);
    }

    .sample-table {
        grid-template-columns: minmax(100px, 0.7fr) minmax(100px, 0.7fr) minmax(170px, 1fr);
    }

    .mapping-notices.blocked {
        border-color: rgb(241 164 164 / 50%);
    }

    .notice-list {
        max-height: 230px;
        overflow: auto;
    }

    .mapping-notice {
        display: grid;
        grid-template-columns: 90px minmax(150px, 0.8fr) minmax(240px, 1.5fr);
        gap: 10px;
        padding: 7px 10px;
        border-top: 1px solid rgb(61 68 72 / 65%);
        font-size: 13px;
    }

    .mapping-notice.notice-blocked,
    .tx16w-footer > span {
        color: var(--color-danger);
    }

    .disposition {
        color: var(--color-text-muted);
    }

    .tx16w-footer {
        justify-content: space-between;
    }

    .tx16w-footer > div {
        gap: 8px;
    }

    @media (max-width: 720px) {
        .object-counts {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 8px;
        }

        .mapping-notice,
        .sample-table,
        .import-mode {
            grid-template-columns: 1fr;
        }
    }
</style>
