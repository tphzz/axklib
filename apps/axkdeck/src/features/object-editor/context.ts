import { getContext, setContext } from 'svelte';
import type { ObjectEditorWorkflow } from './workflow.svelte';
const key = Symbol('device-object-editors');
export function provideObjectEditors(workflow: ObjectEditorWorkflow): void {
    setContext(key, workflow);
}
export function objectEditors(): ObjectEditorWorkflow | undefined {
    return getContext(key);
}
