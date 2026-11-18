export interface ClientUploadSource {
    name: string;
    type: string;
    size: number;
    readonly identity?: string | object;
    readonly sha256?: string;
    nearbySources?(
        selected: ClientUploadSource[],
        excluded: string[],
        signal: AbortSignal,
    ): Promise<{ sources: ClientUploadSource[]; ambiguous: string[] }>;
    readChunk(start: number, end: number): Promise<Blob>;
}

export function browserUploadSource(file: File): ClientUploadSource {
    return {
        name: file.name,
        type: file.type,
        size: file.size,
        identity: file,
        readChunk: async (start, end) => file.slice(start, end),
    };
}
