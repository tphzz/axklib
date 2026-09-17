import type { ObjectDetail } from './transport';

export interface ClipboardWriter {
    writeText(value: string): Promise<void>;
}

export function serializeObjectDetail(detail: ObjectDetail): string {
    return `${JSON.stringify(detail, null, 2)}\n`;
}

export async function copyObjectDetailToClipboard(
    detail: ObjectDetail,
    clipboard: ClipboardWriter | undefined = globalThis.navigator?.clipboard,
): Promise<void> {
    if (!clipboard) throw new Error('Clipboard access is unavailable');
    await clipboard.writeText(serializeObjectDetail(detail));
}
