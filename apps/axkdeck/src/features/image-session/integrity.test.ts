import { describe, expect, it } from 'vitest';
import type { ImageTransport } from '../../lib/transport';
import { nativeWorkspaceFixture } from '../../test/nativeWorkspaceFixture';
import type { ImageSessionWorkflow } from './workflow.svelte';
import { loadIntegrityIssues } from './integrity';

function fixture() {
    const source = nativeWorkspaceFixture().sources[0];
    return {
        sessionId: 1,
        revision: 1,
        documentId: 'shared',
        activeSourceMemberId: 'source-1',
        integrityLoading: false,
        integrityError: '',
        integrityIssues: [],
        sources: [1, 2].map((id) => ({
            ...source,
            id: `source-${id}`,
            diagnostics: [{ code: `issue-${id}`, severity: 'WARNING', message: `Disk ${id}`, path: '/' }],
        })),
    } as unknown as ImageSessionWorkflow;
}

describe('image-scoped integrity', () => {
    it('includes only the active image filesystem diagnostics alongside shared native validation', async () => {
        const workflow = fixture();
        const transport = { validationIssues: async () => [] } as unknown as ImageTransport;
        await loadIntegrityIssues(workflow, transport, 1);
        expect(workflow.integrityIssues.map((issue) => issue.code)).toEqual(['issue-1']);
        workflow.activeSourceMemberId = 'source-2';
        await loadIntegrityIssues(workflow, transport, 1);
        expect(workflow.integrityIssues.map((issue) => issue.code)).toEqual(['issue-2']);
    });

    it('rejects late results from another member even when the document and revision are unchanged', async () => {
        const workflow = fixture();
        let complete!: () => void;
        const transport = {
            validationIssues: async () => {
                await new Promise<void>((resolve) => {
                    complete = resolve;
                });
                return [];
            },
        } as unknown as ImageTransport;
        const pending = loadIntegrityIssues(workflow, transport, 1);
        workflow.activeSourceMemberId = 'source-2';
        workflow.integrityLoading = false;
        complete();
        await pending;
        expect(workflow.integrityIssues).toEqual([]);
        expect(workflow.integrityLoading).toBe(false);
    });
});
