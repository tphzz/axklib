export const tx16wDiskExtensions = new Set(['img', 'ima']);

export function tx16wDiskMediaType(path: string): string | null {
    const extension = path.split('.').at(-1)?.toLocaleLowerCase();
    if (!extension || !tx16wDiskExtensions.has(extension)) {
        return null;
    }
    return 'application/x-raw-disk-image';
}
