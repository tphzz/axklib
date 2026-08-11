export interface ClientUploadSource {
    name: string;
    type: string;
    size: number;
    readChunk(start: number, end: number): Promise<Blob>;
}

export function browserUploadSource(file: File): ClientUploadSource {
    return {
        name: file.name,
        type: file.type,
        size: file.size,
        readChunk: async (start, end) => file.slice(start, end),
    };
}
