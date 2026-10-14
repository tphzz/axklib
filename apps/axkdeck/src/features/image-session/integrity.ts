import type { ImageTransport } from '../../lib/transport';
import type { ImageSessionWorkflow } from './workflow.svelte';
import { userFacingMessage } from '../../lib/userFacingMessage';

export async function loadIntegrityIssues(
    workflow: ImageSessionWorkflow,
    transport: ImageTransport,
    sessionId: number,
    revision = workflow.revision,
): Promise<void> {
    const documentId = workflow.documentId;
    const memberId = workflow.activeSourceMemberId;
    const current = () =>
        workflow.sessionId === sessionId &&
        workflow.revision === revision &&
        workflow.documentId === documentId &&
        workflow.activeSourceMemberId === memberId;
    workflow.integrityLoading = true;
    workflow.integrityError = '';
    try {
        const native = documentId ? await transport.validationIssues(sessionId) : [];
        const issues = [
            ...native,
            ...workflow.sources
                .filter((source) => source.id === memberId)
                .flatMap((source) =>
                    source.diagnostics.map((issue) => ({
                        code: issue.code,
                        severity: issue.severity,
                        message: issue.message,
                        samplerPath: issue.path,
                        objectId: null,
                    })),
                ),
        ];
        if (current()) workflow.integrityIssues = issues;
    } catch (error) {
        if (current()) workflow.integrityError = userFacingMessage(error);
    } finally {
        if (current()) workflow.integrityLoading = false;
    }
}
