import type { ImageLocation } from './storageLocations';
import type { PackageExportSelection } from './types';

const extensions = {
    VOLUME: 'axkvol',
    PROGRAM: 'axkprg',
    SBAC: 'axksbac',
    SBNK: 'axksbnk',
    SMPL: 'axksmpl',
} as const;

function locationBasename(location: ImageLocation): string {
    const referenceName = location.reference.relativePath.replaceAll('\\', '/').split('/').filter(Boolean).at(-1);
    if (referenceName) return referenceName;
    return location.displayName.replaceAll('\\', '/').split('/').filter(Boolean).at(-1) ?? '';
}

function mediaRootName(items: PackageExportSelection[], location: ImageLocation | null): string | undefined {
    if (items.length !== 1 || items[0]?.kind !== 'VOLUME' || !location) return undefined;

    const basename = locationBasename(location);
    if (location.kind === 'axk-object-directory') return basename || undefined;

    const match = /^(.*)\.(?:ima|img)$/i.exec(basename);
    return match?.[1] || undefined;
}

export function packageExportFilename(items: PackageExportSelection[], location: ImageLocation | null = null): string {
    const first = items[0];
    const sourceName =
        items.length === 1
            ? (mediaRootName(items, location) ?? first?.name ?? 'package')
            : `${first?.name ?? 'selection'} and others`;
    const stem =
        sourceName
            .replace(/[<>:"/\\|?*\u0000-\u001f]/g, '_')
            .replace(/[ .]+$/g, '')
            .trim() || 'volume';
    const extension = first && items.every((item) => item.kind === first.kind) ? extensions[first.kind] : 'axkpkg';
    return `${stem}.${extension}`;
}
