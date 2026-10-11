import type { ImageValidationIssue } from '../../lib/transport';

const allocationBlockerCodes = new Set([
    'SFS_ALLOCATION_BITMAP_COPIES_DIFFER',
    'SFS_ALLOCATION_CROSS_LINK',
    'SFS_ALLOCATION_MISMATCH',
    'SFS_EXTENT_BYTE_TOTAL_MISMATCH',
]);

export function allocationBlockerKey(
    issues: ImageValidationIssue[],
    sessionId: number,
    revision: number,
): string | null {
    const codes = [...new Set(issues.map((issue) => issue.code))]
        .filter((code) => allocationBlockerCodes.has(code))
        .sort();
    return codes.length ? `${sessionId}:${revision}:${codes.join(',')}` : null;
}

interface ValidationCounts {
    errorCount: number;
    warningCount: number;
}

function countLabel(count: number, singular: string, plural = `${singular}s`): string {
    return `${count} ${count === 1 ? singular : plural}`;
}

export function validationStatus(validation: ValidationCounts): string {
    if (validation.errorCount > 0 && validation.warningCount > 0) {
        return `${countLabel(validation.errorCount, 'validation error')} · ${countLabel(validation.warningCount, 'warning')}`;
    }
    if (validation.errorCount > 0) return countLabel(validation.errorCount, 'validation error');
    if (validation.warningCount > 0) return countLabel(validation.warningCount, 'validation warning');
    return 'Ready';
}
