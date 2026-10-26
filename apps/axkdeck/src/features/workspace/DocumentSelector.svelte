<script lang="ts">
    import SelectControl from '../../lib/components/SelectControl.svelte';
    import type { OpenedImage } from '../../lib/transport';
    import { userFacingMessage } from '../../lib/userFacingMessage';
    let {
        documents,
        selected,
        discovery,
        disabled,
        onselect,
        onresolve,
    }: {
        documents: OpenedImage['documents'];
        selected: string | null;
        discovery: OpenedImage['discovery'];
        disabled: boolean;
        onselect: (id: string | null) => Promise<void>;
        onresolve: (formats: string[]) => Promise<void>;
    } = $props();
    let formats = $state<string[]>([]);
    let error = $state('');
    let pending = $state(false);
    const notices = $derived(
        discovery.candidates.filter(
            (candidate) =>
                candidate.diagnostics.length > 0 ||
                candidate.state === 'INVALID' ||
                candidate.state === 'LIMIT_EXCEEDED',
        ),
    );
    function dismissible(node: HTMLDetailsElement) {
        const pointer = (event: PointerEvent) => {
            if (!node.contains(event.target as Node)) node.open = false;
        };
        const escape = (event: KeyboardEvent) => {
            if (event.key === 'Escape' && node.open) {
                event.stopPropagation();
                node.open = false;
                node.querySelector('summary')?.focus();
            }
        };
        document.addEventListener('pointerdown', pointer);
        node.addEventListener('keydown', escape);
        return {
            destroy() {
                document.removeEventListener('pointerdown', pointer);
                node.removeEventListener('keydown', escape);
            },
        };
    }
    async function select(id: string) {
        try {
            pending = true;
            error = '';
            await onselect(id || null);
        } catch (reason) {
            error = userFacingMessage(reason);
        } finally {
            pending = false;
        }
    }
    async function resolve() {
        try {
            pending = true;
            error = '';
            await onresolve(formats);
        } catch (reason) {
            error = userFacingMessage(reason);
        } finally {
            pending = false;
        }
    }
</script>

{#if documents.length > 1}
    <div class="document-select">
        <SelectControl
            label="Native document"
            value={selected ?? ''}
            placeholder="Choose native format"
            size="header"
            options={documents.map((document) => ({
                value: document.id,
                label: `${document.name} / ${document.profile}${document.completeness === 'INCOMPLETE' ? ' (incomplete)' : ''}`,
            }))}
            disabled={disabled || pending}
            onchange={(id) => void select(id)}
        />
    </div>
{/if}
{#if discovery.selectionRequired}
    <details class="format-selection" use:dismissible>
        <summary>Choose native formats</summary>
        <div>
            {#each discovery.candidates.filter((item) => item.state === 'SUPPORTED' || item.state === 'INCOMPLETE') as candidate (candidate.formatId)}
                <label
                    ><input
                        class="dialog-checkbox"
                        type="checkbox"
                        disabled={disabled || pending}
                        checked={formats.includes(candidate.formatId)}
                        onchange={(event) =>
                            (formats = event.currentTarget.checked
                                ? [...formats, candidate.formatId]
                                : formats.filter((id) => id !== candidate.formatId))}
                    />{candidate.formatId}</label
                >
            {/each}
            <button
                class="secondary-button"
                type="button"
                disabled={disabled || pending || !formats.length}
                onclick={() => void resolve()}>Apply</button
            >
        </div>
    </details>
{/if}
{#if notices.length}
    <details class="format-selection" use:dismissible>
        <summary>Discovery notices ({notices.length})</summary>
        <div>
            {#each notices as candidate (candidate.formatId)}
                <strong>{candidate.formatId}: {candidate.state}</strong>
                {#each candidate.diagnostics as message}<p>{message}</p>{/each}
            {/each}
        </div>
    </details>
{/if}
{#if error}<p class="document-error" role="alert">{error}</p>{/if}

<style>
    .document-select {
        width: 100%;
        min-width: 0;
        max-width: 320px;
        height: 25px;
        font-size: 11px;
    }
    .format-selection {
        position: relative;
        font-size: 11px;
    }
    .format-selection > div {
        position: absolute;
        z-index: 20;
        top: 100%;
        padding: 8px;
        width: 300px;
        max-width: calc(100vw - 32px);
        max-height: 200px;
        overflow: auto;
        background: var(--color-panel);
        border: 1px solid var(--color-border);
    }
    .document-error {
        font-size: 11px;
        color: var(--color-danger);
        margin: 0;
    }
    label {
        display: flex;
        align-items: center;
        gap: 5px;
        white-space: nowrap;
        padding: 4px 0;
    }
</style>
