import { formatStoredSize } from './formatBytes';

interface SizedObject {
    storedSizeBytes: number;
    sizeWithDependenciesBytes: number | null;
}

export function objectSizeSummary(object: SizedObject): string {
    const dependencySize = object.sizeWithDependenciesBytes;
    return dependencySize === null
        ? `${formatStoredSize(object.storedSizeBytes)} · deps. unavailable`
        : `${formatStoredSize(object.storedSizeBytes)} · ${formatStoredSize(dependencySize)} incl. deps.`;
}

export function objectSizeTooltip(object: SizedObject): string {
    const dependencySize = object.sizeWithDependenciesBytes;
    return [
        `Object size: ${formatStoredSize(object.storedSizeBytes)}`,
        `Object size with deps.: ${dependencySize === null ? 'Unavailable' : formatStoredSize(dependencySize)}`,
    ].join('\n');
}
