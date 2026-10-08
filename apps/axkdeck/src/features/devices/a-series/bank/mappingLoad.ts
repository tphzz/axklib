import type { ObjectEditorDocument, ObjectEditorWorkflow } from '../../../object-editor/workflow.svelte';
import { mappingMembers } from './mappingMembers';

interface Pool {
    active: number;
    waiting: (() => void)[];
    requests: Map<string, Promise<ObjectEditorDocument>>;
}
const pools = new WeakMap<ObjectEditorWorkflow, Pool>();

export function loadMappingMember(
    editors: ObjectEditorWorkflow,
    bank: ObjectEditorDocument,
    id: string,
): Promise<ObjectEditorDocument> {
    const detail = bank.detail;
    if (!detail || !mappingMembers(detail).some((member) => member.id === id))
        return Promise.reject(new Error('This Sample is not a confirmed member of the bank'));
    const revision = detail.image.revision;
    let pool = pools.get(editors);
    if (!pool) {
        pool = { active: 0, waiting: [], requests: new Map() };
        pools.set(editors, pool);
    }
    const shared = pool;
    const key = `${bank.sessionId}:${revision}:${id}`;
    let request = shared.requests.get(key);
    if (!request) {
        request = (async () => {
            if (shared.active >= 4) await new Promise<void>((resolve) => shared.waiting.push(resolve));
            else shared.active++;
            try {
                const document = await editors.load(bank.sessionId, id);
                if (document && document.detail?.image.revision !== revision && !(await editors.check(document)))
                    throw new Error(`${id}: Sample mapping changed outside the editor`);
                if (
                    !document ||
                    document.detail?.image.revision !== revision ||
                    document.detail.editing?.profile !== 'a-series/sample'
                )
                    throw new Error(`${id}: Sample mapping is unavailable at this image revision`);
                return document;
            } finally {
                const next = shared.waiting.shift();
                if (next) next();
                else shared.active--;
            }
        })();
        shared.requests.set(key, request);
        void request.finally(() => shared.requests.delete(key)).catch(() => {});
    }
    return request;
}

export async function loadMappingMembers(
    editors: ObjectEditorWorkflow,
    bank: ObjectEditorDocument,
    current: () => boolean,
) {
    const detail = bank.detail;
    if (!detail) return;
    const members = mappingMembers(detail);
    for (let offset = 0; offset < members.length; offset += 4) {
        if (!current()) return;
        await Promise.all(
            members.slice(offset, offset + 4).map((member) => loadMappingMember(editors, bank, member.id)),
        );
    }
}
