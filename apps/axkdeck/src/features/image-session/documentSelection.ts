import type { ImageTransport, OpenedImage } from '../../lib/transport';
import type { ImageSessionWorkflow } from './workflow.svelte';
import type { SessionCollaborators } from './collaborators';
import { userFacingMessage } from '../../lib/userFacingMessage';

export function documentBindings(workflow: ImageSessionWorkflow) {
    return {
        documents: workflow.documents,
        documentId: workflow.documentId,
        sources: workflow.sources,
        discovery: workflow.discovery,
        switchingDocument: workflow.switchingDocument,
        documentSwitchBlocked: !workflow.canSwitchDocument(),
        selectDocument: (id: string | null) => workflow.selectDocument(id),
        resolveDocuments: (formats: string[]) => workflow.resolveDocuments(formats),
    };
}

export class DocumentSelection {
    private preferences = new Map<string, ReturnType<ImageSessionWorkflow['currentSourcePreference']>>();
    private states = new Map<string, () => void>();
    clear() {
        this.preferences.clear();
        this.states.clear();
    }

    async change(
        workflow: ImageSessionWorkflow,
        collaborators: SessionCollaborators,
        transport: ImageTransport,
        request: { documentId: string | null } | { formats: string[] },
        apply: (
            opened: OpenedImage,
            preferred?: ReturnType<ImageSessionWorkflow['currentSourcePreference']>,
        ) => Promise<void>,
    ) {
        if (
            workflow.sessionId === null ||
            workflow.opening ||
            workflow.switchingDocument ||
            !workflow.canSwitchDocument() ||
            collaborators.mutation.volumeActionLocked ||
            collaborators.mutation.objectRenameBusy
        )
            return;
        if ('documentId' in request && request.documentId === workflow.documentId) return;
        workflow.switchingDocument = true;
        const sessionId = workflow.sessionId;
        try {
            if (
                !(await workflow.confirmEditorLeave()) ||
                workflow.sessionId !== sessionId ||
                !workflow.canSwitchDocument()
            )
                return;
            if (workflow.documentId) {
                this.preferences.set(workflow.documentId, workflow.currentSourcePreference());
                const restore = collaborators.captureDocumentState?.();
                if (restore) this.states.set(workflow.documentId, restore);
            }
            await collaborators.audition.invalidateSession(sessionId);
            const opened =
                'formats' in request
                    ? await transport.resolveDocuments(sessionId, request.formats)
                    : await transport.selectDocument(sessionId, request.documentId);
            if (workflow.sessionId !== sessionId) return;
            collaborators.catalog.clear();
            collaborators.resetDocumentState?.();
            await apply(opened, opened.documentId ? this.preferences.get(opened.documentId) : undefined);
            if (workflow.sessionId === sessionId && workflow.documentId === opened.documentId && opened.documentId)
                this.states.get(opened.documentId)?.();
        } catch (reason) {
            if (workflow.sessionId === sessionId) workflow.status = userFacingMessage(reason);
        } finally {
            workflow.switchingDocument = false;
        }
    }
}
