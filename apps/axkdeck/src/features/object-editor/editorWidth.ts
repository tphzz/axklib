import { getContext, setContext } from 'svelte';

const key = Symbol('editor-width');
export function provideEditorWidth(read: () => number) {
    setContext(key, read);
}
export function initialEditorWidth(): number {
    return getContext<(() => number) | undefined>(key)?.() ?? 0;
}
