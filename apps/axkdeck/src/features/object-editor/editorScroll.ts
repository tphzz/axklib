import { tick } from 'svelte';

type ScrollPosition = { top: number; left: number };
export type EditorScroll = { positions: Map<string, ScrollPosition>; key: string };
export const editorScrollContext = Symbol('editor-scroll');

export function rememberEditorScroll(node: HTMLElement, initial: EditorScroll | undefined) {
    let current = initial;
    let revision = 0;
    let restoring = false;
    function save() {
        if (current && !restoring) current.positions.set(current.key, { top: node.scrollTop, left: node.scrollLeft });
    }
    async function restore() {
        const version = ++revision;
        restoring = true;
        await tick();
        if (version !== revision) return;
        const position = current?.positions.get(current.key);
        node.scrollTop = position?.top ?? 0;
        node.scrollLeft = position?.left ?? 0;
        restoring = false;
    }
    node.addEventListener('scroll', save);
    void restore();
    return {
        update(next: EditorScroll | undefined) {
            if (next?.key === current?.key && next?.positions === current?.positions) return;
            current = next;
            void restore();
        },
        destroy() {
            revision++;
            node.removeEventListener('scroll', save);
        },
    };
}
