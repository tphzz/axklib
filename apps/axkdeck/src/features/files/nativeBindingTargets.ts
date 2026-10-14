import type { FilesystemBinding } from '../../lib/filesystem';
import type { OpenedImage } from '../../lib/transport';

export function nativeBindingTargets(bindings: FilesystemBinding[], documents: OpenedImage['documents']) {
    return documents.flatMap((document) => {
        const matches = bindings.filter(
            (binding) => binding.documentId === document.id && (binding.objectId || binding.contentScopeId),
        );
        if (!matches.length) return [];
        const objects = new Set(matches.flatMap((binding) => (binding.objectId ? [binding.objectId] : [])));
        const scopes = new Set(matches.map((binding) => binding.contentScopeId));
        // A container file can represent many native objects; never choose its first object implicitly.
        const binding: FilesystemBinding = {
            documentId: document.id,
            objectId: objects.size === 1 ? [...objects][0] : null,
            contentScopeId: scopes.size === 1 ? matches[0].contentScopeId : null,
        };
        return [{ binding, label: `Show in ${document.name}${objects.size > 1 ? ` (${objects.size} objects)` : ''}` }];
    });
}
