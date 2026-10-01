import { GraphViewport } from '../../../object-editor/graphViewport.svelte';
import type { EditorDraft } from '../../../object-editor/draft.svelte';
import { envelopeDuration, type SampleEnvelope } from './envelope';

const views = new WeakMap<EditorDraft, Map<SampleEnvelope, GraphViewport>>();
export function envelopeViewport(draft: EditorDraft, kind: SampleEnvelope, extent: number): GraphViewport {
    let group = views.get(draft);
    if (!group) views.set(draft, (group = new Map()));
    let view = group.get(kind);
    if (!view) group.set(kind, (view = new GraphViewport(extent, 16, 32 + 3 * envelopeDuration(0))));
    return view;
}
