import type { ImageTransport, OpenedImage } from '../../lib/transport';
import type { ImageSessionWorkflow } from './workflow.svelte';
import type { SessionCollaborators } from './collaborators';
import { userFacingMessage } from '../../lib/userFacingMessage';
import { applyOpenedImageMetadata } from './openedImageMetadata';

export function documentBindings(workflow: ImageSessionWorkflow) {
    return {
        documents: workflow.documents,
        documentId: workflow.documentId,
        sources: workflow.sources,
        activeSourceMemberId: workflow.activeSourceMemberId,
        discovery: workflow.discovery,
        switchingDocument: workflow.switchingDocument,
        documentSwitchBlocked: !workflow.canSwitchDocument(),
        selectDocument: (id: string | null, memberId?: string) => workflow.selectDocument(id, memberId),
        selectImage: (id: string) => workflow.selectDocument(undefined, id),
        resolveDocuments: (formats: string[]) => workflow.resolveDocuments(formats),
    };
}

export class DocumentSelection {
    private preferences = new Map<string, ReturnType<ImageSessionWorkflow['currentSourcePreference']>>();
    private states = new Map<string, () => void>();
    private documents = new Map<string, string>();
    clear() {
        this.preferences.clear();
        this.states.clear();
        this.documents.clear();
    }

    async change(
        workflow: ImageSessionWorkflow,
        collaborators: SessionCollaborators,
        transport: ImageTransport,
        request: { documentId: string | null | undefined; sourceMemberId?: string } | { formats: string[] },
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
        if (
            'documentId' in request &&
            (request.sourceMemberId
                ? request.sourceMemberId === workflow.activeSourceMemberId && request.documentId === undefined
                : request.documentId === workflow.documentId)
        )
            return;
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
                if (workflow.activeSourceMemberId)
                    this.documents.set(workflow.activeSourceMemberId, workflow.documentId);
                this.preferences.set(workflow.documentId, workflow.currentSourcePreference());
                const restore = collaborators.captureDocumentState?.();
                if (restore) this.states.set(workflow.documentId, restore);
            }
            await collaborators.audition.invalidateSession(sessionId);
            if (workflow.sessionId !== sessionId) return;
            const memberId = 'sourceMemberId' in request ? request.sourceMemberId : undefined;
            const remembered = memberId ? this.documents.get(memberId) : undefined;
            let documentId = 'documentId' in request ? request.documentId : undefined;
            if (
                memberId &&
                documentId === undefined &&
                workflow.documents.some((item) => item.id === remembered && item.sourceMemberIds.includes(memberId))
            )
                documentId = remembered;
            const opened =
                'formats' in request
                    ? await transport.resolveDocuments(sessionId, request.formats)
                    : await transport.selectDocument(sessionId, documentId, memberId);
            if (workflow.sessionId !== sessionId) return;
            if (
                'documentId' in request &&
                opened.documentId === workflow.documentId &&
                opened.revision === workflow.revision
            ) {
                applyOpenedImageMetadata(workflow, opened);
                return;
            }
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
