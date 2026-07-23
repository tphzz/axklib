interface Page<T> {
    items: T[];
    totalCount: number;
}

interface PaginationOptions<T> {
    pageSize?: number;
    maximumPages?: number;
    key: (item: T) => string;
    cancelled?: () => boolean;
}

export async function collectPages<T>(
    fetchPage: (offset: number, limit: number) => Promise<Page<T>>,
    options: PaginationOptions<T>,
): Promise<T[]> {
    const pageSize = options.pageSize ?? 256;
    const maximumPages = options.maximumPages ?? 4096;
    if (!Number.isSafeInteger(pageSize) || pageSize <= 0 || !Number.isSafeInteger(maximumPages) || maximumPages <= 0) {
        throw new Error('Invalid pagination bounds');
    }

    const items: T[] = [];
    const keys = new Set<string>();
    let expectedTotal: number | null = null;
    for (let pageNumber = 0; pageNumber < maximumPages; pageNumber += 1) {
        if (options.cancelled?.()) throw new Error('Pagination was cancelled');
        const page = await fetchPage(items.length, pageSize);
        if (!Number.isSafeInteger(page.totalCount) || page.totalCount < 0) {
            throw new Error('Server returned an invalid pagination total');
        }
        if (expectedTotal === null) expectedTotal = page.totalCount;
        else if (page.totalCount !== expectedTotal) throw new Error('Server pagination total changed during loading');
        if (items.length + page.items.length > expectedTotal) {
            throw new Error('Server returned more paginated items than declared');
        }
        if (page.items.length === 0 && items.length < expectedTotal) {
            throw new Error('Server pagination made no forward progress');
        }
        for (const item of page.items) {
            const key = options.key(item);
            if (keys.has(key)) throw new Error('Server pagination repeated an item');
            keys.add(key);
            items.push(item);
        }
        if (items.length === expectedTotal) return items;
    }
    throw new Error('Server pagination exceeded the request limit');
}
