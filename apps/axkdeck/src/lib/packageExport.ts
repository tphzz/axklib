import type { PackageExportSelection } from './types';

const extensions = {
    VOLUME: 'axkvol',
    PROGRAM: 'axkprg',
    SBAC: 'axksbac',
    SBNK: 'axksbnk',
    SMPL: 'axksmpl',
} as const;

export function packageExportFilename(items: PackageExportSelection[]): string {
    const first = items[0];
    const sourceName = items.length === 1 ? (first?.name ?? 'package') : `${first?.name ?? 'selection'} and others`;
    const stem =
        sourceName
            .replace(/[<>:"/\\|?*\u0000-\u001f]/g, '_')
            .replace(/[ .]+$/g, '')
            .trim() || 'volume';
    const extension = first && items.every((item) => item.kind === first.kind) ? extensions[first.kind] : 'axkpkg';
    return `${stem}.${extension}`;
}
