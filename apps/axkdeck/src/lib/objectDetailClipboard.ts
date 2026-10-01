import type { ObjectDetail } from './transport';
import type { ImageTransport } from './transport';
import { userFacingMessage } from './userFacingMessage';

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

export async function copySessionObjectMetadata(
    transport: ImageTransport,
    sessionId: number | null,
    objectId: string,
    setStatus: (message: string) => void,
): Promise<void> {
    if (sessionId === null) throw new Error('No image is open');
    try {
        await copyObjectDetailToClipboard(await transport.objectDetail(sessionId, objectId));
        setStatus('Copied object metadata to the clipboard');
    } catch (error) {
        setStatus(userFacingMessage(error));
        throw error;
    }
}
