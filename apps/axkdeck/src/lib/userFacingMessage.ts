export function userFacingMessage(reason: unknown): string {
    const message = reason instanceof Error ? reason.message : String(reason);
    if (!message || message.startsWith('axklib')) return message;
    const first = message[0];
    return first >= 'a' && first <= 'z' ? first.toLocaleUpperCase() + message.slice(1) : message;
}
