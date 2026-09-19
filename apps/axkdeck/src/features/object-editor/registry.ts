import { aSeriesSampleAdapter } from '../devices/a-series/sample/adapter';
import type { ObjectDetail } from '../../lib/transport';

// Editor profiles describe device/object semantics, independently of server backends.
export function objectEditorAdapter(detail: ObjectDetail) {
    return detail.editing?.profile === aSeriesSampleAdapter.profile ? aSeriesSampleAdapter : null;
}

export async function prepareEditorDraft(
    ...args: Parameters<typeof import('../devices/a-series/sample/audition').prepareSampleDraft>
) {
    if (args[3].profile !== aSeriesSampleAdapter.profile)
        throw new Error('Draft preview is unavailable for this profile');
    const { prepareSampleDraft } = await import('../devices/a-series/sample/audition');
    return prepareSampleDraft(...args);
}
