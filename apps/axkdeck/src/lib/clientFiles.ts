import type { ClientDownload } from './transport';

export function chooseClientFile(accept: string): Promise<File | null> {
    return new Promise((resolve) => {
        const input = document.createElement('input');
        input.type = 'file';
        input.accept = accept;
        input.multiple = false;
        input.style.display = 'none';
        const finish = (file: File | null): void => {
            input.remove();
            resolve(file);
        };
        input.addEventListener('change', () => finish(input.files?.item(0) ?? null), { once: true });
        input.addEventListener('cancel', () => finish(null), { once: true });
        document.body.append(input);
        input.click();
    });
}

export function saveClientDownload(download: ClientDownload): void {
    const url = URL.createObjectURL(download.blob);
    const anchor = document.createElement('a');
    anchor.href = url;
    anchor.download = download.filename;
    document.body.append(anchor);
    anchor.click();
    anchor.remove();
    URL.revokeObjectURL(url);
}
