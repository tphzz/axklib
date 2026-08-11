interface NamedItem {
    id: string;
    name: string;
}

const naturalNameCollator = new Intl.Collator('en', {
    numeric: true,
    sensitivity: 'base',
});

function compareExactText(left: string, right: string): number {
    return left < right ? -1 : left > right ? 1 : 0;
}

export function compareNaturalNames(left: string, right: string): number {
    return naturalNameCollator.compare(left, right) || compareExactText(left, right);
}

export function compareNamedItems(left: NamedItem, right: NamedItem): number {
    return compareNaturalNames(left.name, right.name) || compareExactText(left.id, right.id);
}
