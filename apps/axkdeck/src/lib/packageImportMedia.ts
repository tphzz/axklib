export const packageImportExtensions = [
    'a3k',
    'axkvol',
    'axkprg',
    'axksbac',
    'axksbnk',
    'axksmpl',
    'axkseq',
    'axkpkg',
] as const;

const packageImportExtensionSet = new Set<string>(packageImportExtensions);

export function packageImportUploadKind(filename: string): 'PACKAGE' | 'DISK_IMAGE' | null {
    const extension = filename.split('.').pop()?.toLowerCase() ?? '';
    if (!packageImportExtensionSet.has(extension)) return null;
    return extension === 'a3k' ? 'DISK_IMAGE' : 'PACKAGE';
}

export function packageImportExtensionSetCopy(): Set<string> {
    return new Set(packageImportExtensions);
}
