<script lang="ts">
    import { onMount, untrack, type Snippet } from 'svelte';
    import type { ImageTransport } from '../../lib/transport';
    import type { ImageSessionWorkflow } from '../image-session/workflow.svelte';
    import type { AuditionWorkflow } from '../audition/workflow.svelte';
    import { ObjectEditorWorkflow } from './workflow.svelte';
    import { provideObjectEditors } from './context';
    import { provideEditorAudio } from './audioContext';
    import SampleDuplicateDialog from './SampleDuplicateDialog.svelte';
    import SampleFormatDialog from './SampleFormatDialog.svelte';
    import { modal } from '../../lib/modal';
    import { prepareEditorDraft } from './registry';
    import type { installDesktopEditorGuard } from './desktopGuard';
    let {
        transport,
        imageSession,
        audition,
        children,
    }: {
        transport: ImageTransport;
        imageSession: ImageSessionWorkflow;
        audition: AuditionWorkflow;
        children: Snippet;
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
            const values = { ...document.draft.values };
            return audition.playPrepared(document.sessionId, document.detail!.object.id, (context, signal) =>
                prepareEditorDraft(
                    transport,
                    document.sessionId,
                    document.detail!.object.id,
                    document.detail!.editing!,
                    values,
                    note,
                    context,
                    signal,
                ),
            );
        },
    });
    provideObjectEditors(editors);
    provideEditorAudio({
        get transport() {
            return transport;
        },
        get audition() {
            return audition;
        },
    });
    let confirmation = $state<((discard: boolean) => void) | null>(null);
    let desktop = $state.raw<Awaited<ReturnType<typeof installDesktopEditorGuard>>>();
    let previousSession: number | null = null;
    async function confirmLeave(): Promise<boolean> {
        if (editors.locked) {
            imageSession.setStatus('Resolve the pending Sample save before closing the image');
            return false;
        }
        if (!editors.dirtyCount) return true;
        if (confirmation) return false;
        return new Promise((resolve) => {
            confirmation = (discard) => {
                confirmation = null;
                if (discard) editors.clear();
                resolve(discard);
            };
        });
    }
    $effect.pre(() => {
        const session = imageSession.sessionId;
        const revision = imageSession.revision;
        untrack(() => {
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
        };
    });
    $effect(() => {
        desktop?.setBlocked(editors.dirtyCount > 0 || editors.locked);
    });
</script>

{@render children()}
{#if editors.duplication.visible}<SampleDuplicateDialog workflow={editors.duplication} />{/if}
{#if editors.conversionDocument}<SampleFormatDialog workflow={editors} document={editors.conversionDocument} />{/if}
{#if confirmation}
    <div class="dialog-backdrop dialog-backdrop-top" role="presentation">
        <div
            class="dialog-shell draft-confirmation"
            role="dialog"
            aria-modal="true"
            aria-labelledby="draft-close-title"
            use:modal={{ onescape: () => confirmation?.(false) }}
        >
            <header class="dialog-header"><h2 id="draft-close-title">Unsaved Sample edits</h2></header>
            <div class="draft-message">
                {editors.dirtyCount} Sample draft{editors.dirtyCount === 1 ? '' : 's'} will be discarded.
            </div>
            <footer class="dialog-footer">
                <span class="dialog-footer-status"></span>
                <div class="dialog-footer-actions">
                    <button class="secondary-button" onclick={() => confirmation?.(false)}>Cancel</button>
                    <button class="danger-button" onclick={() => confirmation?.(true)}>Discard and continue</button>
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
