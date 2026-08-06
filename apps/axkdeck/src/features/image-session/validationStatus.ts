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
