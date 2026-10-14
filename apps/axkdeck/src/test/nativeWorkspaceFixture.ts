import type { OpenedImage } from '../lib/transport';

export function nativeWorkspaceFixture(): Pick<OpenedImage, 'sources' | 'documents' | 'documentId' | 'discovery'> {
    return {
        sources: [
            {
                id: 'source-1',
                source: { kind: 'FILE', file: { rootId: 'root', relativePath: 'test.hds' } },
                format: 'sfs',
                filesystemRootIds: ['source-1:sfs-0'],
                diagnostics: [],
            },
        ],
        documents: [
            {
                id: 'document-a-series-source-1',
                formatId: 'yamaha.a-series',
                profile: 'a-series',
                name: 'A-series',
                completeness: 'COMPLETE',
                format: 'sfs',
                floppySet: null,
                companionSources: [],
                validation: { valid: true, infoCount: 0, warningCount: 0, errorCount: 0 },
                sourceMemberIds: ['source-1'],
                availableOperations: [],
                rootCount: 1,
                objectCount: 0,
                relationshipCount: 0,
            },
        ],
        documentId: 'document-a-series-source-1',
        discovery: { selectionRequired: false, candidates: [] },
    };
}
