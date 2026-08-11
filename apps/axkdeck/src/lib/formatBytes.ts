export function formatStoredSize(bytes: number): string {
    if (bytes >= 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} MiB`;
    if (bytes >= 1024) return `${Math.ceil(bytes / 1024)} KiB`;
    return `${bytes} B`;
}
