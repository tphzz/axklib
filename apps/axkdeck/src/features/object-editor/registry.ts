import { aSeriesSampleAdapter } from '../devices/a-series/sample/adapter';
import { aSeriesBankAdapter } from '../devices/a-series/bank/adapter';
import type { ObjectDetail } from '../../lib/transport';

// Editor profiles describe device/object semantics, independently of server backends.
export function objectEditorAdapter(detail: ObjectDetail) {
    return detail.editing?.profile === aSeriesSampleAdapter.profile
        ? aSeriesSampleAdapter
        : detail.editing?.profile === aSeriesBankAdapter.profile && detail.editing.bankOverrides
          ? aSeriesBankAdapter
          : null;
}

export async function prepareEditorDraft(
    ...args: Parameters<typeof import('../devices/a-series/sample/audition').prepareSampleDraft>
) {
    if (args[3].profile !== aSeriesSampleAdapter.profile)
        throw new Error('Draft preview is unavailable for this profile');
    const { prepareSampleDraft } = await import('../devices/a-series/sample/audition');
    return prepareSampleDraft(...args);
}
