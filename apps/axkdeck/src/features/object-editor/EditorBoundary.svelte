<script lang="ts">
    import { onMount, untrack, type Snippet } from 'svelte';
    import type { ImageTransport } from '../../lib/transport';
    import type { ImageSessionWorkflow } from '../image-session/workflow.svelte';
    import type { AuditionWorkflow } from '../audition/workflow.svelte';
    import { ObjectEditorWorkflow } from './workflow.svelte';
    import { provideObjectEditors } from './context';
    import { provideEditorAudio } from './audioContext';
    import SampleDuplicateDialog from './SampleDuplicateDialog.svelte';
    import ObjectFormatDialog from './ObjectFormatDialog.svelte';
    import { modal } from '../../lib/modal';
    import { prepareEditorDraft } from './registry';
    import { userFacingMessage } from '../../lib/userFacingMessage';
    import type { installDesktopEditorGuard } from './desktopGuard';
    import type { InspectorSelection } from '../../lib/types';
    import { MappingController, provideMappingEditor } from '../program-mapping/controller.svelte';
    import MappingBridge from '../program-mapping/MappingBridge.svelte';
    import { mappingRoles } from '../program-mapping/protocol';
    import { MappingAudition } from '../program-mapping/audition';
    let {
        transport,
        imageSession,
        audition,
        children,
        selection = null,
    }: {
        transport: ImageTransport;
        imageSession: ImageSessionWorkflow;
        audition: AuditionWorkflow;
        children: Snippet;
        selection?: InspectorSelection;
    } = $props();
    const editors = new ObjectEditorWorkflow({
        transport: untrack(() => transport),
        refresh: () =>
            audition.refreshEditorWorkspace(() =>
                imageSession.refresh(imageSession.currentSourcePreference(), 'editor'),
            ),
        stopPlayback: () => {
            void audition.stop();
        },
        status: (text) => imageSession.setStatus(text),
        audition: (document, note) => {
            const snapshot = document.detail?.editing;
            if (snapshot?.profile !== 'a-series/sample')
                return Promise.reject(new Error('Sample audition is unavailable'));
            const values = { ...document.draft.values };
            return audition.playPrepared(document.sessionId, document.detail!.object.id, (context, signal) =>
                prepareEditorDraft(
                    transport,
                    document.sessionId,
                    document.detail!.object.id,
                    snapshot,
                    values,
                    note,
                    context,
                    signal,
                ),
            );
        },
    });
    provideObjectEditors(editors);
    const mappingAudio = new MappingAudition(
        editors,
        untrack(() => transport),
        untrack(() => audition),
    );
    const mappings = mappingRoles.map((role) => new MappingController(editors, role, mappingAudio));
    mappings.forEach(provideMappingEditor);
    provideEditorAudio({
        mapping: mappingAudio,
        get transport() {
            return transport;
        },
        get audition() {
            return audition;
        },
    });
    let confirmation = $state<((discard: boolean) => void) | null>(null);
    let savingDrafts = $state(false);
    let discardingDrafts = $state(false);
    let confirmationError = $state('');
    let desktop = $state.raw<Awaited<ReturnType<typeof installDesktopEditorGuard>>>();
    let previousSession: number | null = null;
    async function confirmLeave(): Promise<boolean> {
        if (editors.locked) {
            imageSession.setStatus('Resolve the pending editor save before continuing');
            return false;
        }
        if (!editors.dirtyCount) return true;
        if (confirmation) return false;
        confirmationError = '';
        return new Promise((resolve) => {
            confirmation = (discard) => {
                confirmation = null;
                resolve(discard);
            };
        });
    }
    async function discardAndContinue(): Promise<void> {
        if (savingDrafts || !confirmation) return;
        savingDrafts = true;
        discardingDrafts = true;
        try {
            if (await editors.discardAll()) confirmation?.(true);
            else confirmationError = 'Discard did not complete. Resolve the editor status before continuing.';
        } catch (error) {
            confirmationError = userFacingMessage(error);
        } finally {
            savingDrafts = false;
            discardingDrafts = false;
        }
    }
    async function saveAndContinue(): Promise<void> {
        if (savingDrafts || !confirmation) return;
        savingDrafts = true;
        try {
            if (await editors.saveAll()) confirmation?.(true);
            else confirmationError = 'Save did not complete. Resolve the editor status before continuing.';
        } catch (error) {
            confirmationError = userFacingMessage(error);
        } finally {
            savingDrafts = false;
        }
    }
    $effect.pre(() => {
        const session = imageSession.sessionId;
        const revision = imageSession.revision;
        untrack(() => {
            mappingAudio.invalidate();
            if (previousSession !== session) {
                editors.clear();
                previousSession = session;
            }
            if (session === null) editors.clear();
            else if (revision) void editors.revalidate(session);
        });
    });
    onMount(() => {
        imageSession.confirmEditorLeave = confirmLeave;
        editors.confirmStructuralChange = confirmLeave;
        let disposed = false;
        const beforeUnload = (event: BeforeUnloadEvent) => {
            if (editors.dirtyCount || editors.locked) {
                event.preventDefault();
                event.returnValue = '';
            }
        };
        window.addEventListener('beforeunload', beforeUnload);
        if ('__TAURI_INTERNALS__' in window)
            void import('./desktopGuard')
                .then(async ({ installDesktopEditorGuard }) => {
                    const guard = await installDesktopEditorGuard(confirmLeave, () =>
                        imageSession.setStatus('Desktop exit protection failed; save edits before quitting'),
                    );
                    if (disposed) guard.dispose();
                    else desktop = guard;
                })
                .catch(() => {
                    if (!disposed)
                        imageSession.setStatus(
                            'Desktop close protection could not be initialized; save or discard edits before quitting',
                        );
                });
        return () => {
            disposed = true;
            desktop?.dispose();
            window.removeEventListener('beforeunload', beforeUnload);
            imageSession.confirmEditorLeave = async () => true;
            confirmation?.(false);
            mappingAudio.invalidate();
        };
    });
    $effect(() => {
        desktop?.setBlocked(editors.dirtyCount > 0 || editors.locked);
    });
</script>

{#each mappings as controller}<MappingBridge
        {controller}
        {editors}
        sessionId={imageSession.sessionId}
        {selection}
    />{/each}
{@render children()}
{#if editors.duplication.visible}<SampleDuplicateDialog workflow={editors.duplication} />{/if}
{#if editors.conversionDocument}<ObjectFormatDialog workflow={editors} document={editors.conversionDocument} />{/if}
{#if confirmation}
    <div class="dialog-backdrop dialog-backdrop-top" role="presentation">
        <div
            class="dialog-shell draft-confirmation"
            role="dialog"
            aria-modal="true"
            aria-labelledby="draft-close-title"
            use:modal={{
                onescape: () => {
                    if (!savingDrafts) confirmation?.(false);
                },
            }}
        >
            <header class="dialog-header"><h2 id="draft-close-title">Unsaved edits</h2></header>
            <div class="draft-message">
                {editors.dirtyCount} unsaved draft{editors.dirtyCount === 1 ? '' : 's'}.
            </div>
            <footer class="dialog-footer">
                <span class="dialog-footer-status" role="status"
                    >{savingDrafts ? (discardingDrafts ? 'Discarding' : 'Saving') : confirmationError}</span
                >
                <div class="dialog-footer-actions">
                    <button class="secondary-button" disabled={savingDrafts} onclick={() => confirmation?.(false)}
                        >Cancel</button
                    >
                    <button
                        class="danger-button"
                        disabled={savingDrafts || editors.locked}
                        onclick={() => void discardAndContinue()}>Discard</button
                    >
                    <button
                        class="primary-button"
                        disabled={savingDrafts || editors.locked}
                        onclick={() => void saveAndContinue()}>Save</button
                    >
                </div>
            </footer>
        </div>
    </div>
{/if}

<style>
    .draft-confirmation {
        width: min(460px, calc(100vw - 32px));
    }
    .draft-message {
        padding: 20px 16px;
    }
</style>
