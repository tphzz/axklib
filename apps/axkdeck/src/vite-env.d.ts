/// <reference types="vite/client" />

interface Window {
    __AXKLIB_SERVER__?: {
        baseUrl: string;
        bearerToken: string;
        mode: 'local' | 'remote';
    };
}
