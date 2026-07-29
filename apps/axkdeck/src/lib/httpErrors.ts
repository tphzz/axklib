export class AxklibApiError extends Error {
    readonly code: string;
    readonly status: number;
    readonly requestId?: string;
    readonly context?: unknown;
    readonly retryable: boolean;

    constructor(
        code: string,
        message: string,
        status: number,
        requestId?: string,
        context?: unknown,
        retryable = false,
    ) {
        super(message);
        this.name = 'AxklibApiError';
        this.code = code;
        this.status = status;
        this.requestId = requestId;
        this.context = context;
        this.retryable = retryable;
    }
}
