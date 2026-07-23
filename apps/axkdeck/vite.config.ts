import tailwindcss from '@tailwindcss/vite';
import { svelte } from '@sveltejs/vite-plugin-svelte';
import { defineConfig } from 'vitest/config';

const tauriHost = process.env.TAURI_DEV_HOST;

export default defineConfig({
    plugins: [svelte(), tailwindcss()],
    resolve: {
        conditions: ['browser'],
    },
    test: {
        environment: 'jsdom',
        include: ['src/**/*.test.ts'],
        setupFiles: ['./src/test/setup.ts'],
    },
    clearScreen: false,
    server: {
        host: tauriHost ?? false,
        port: 5173,
        strictPort: true,
        hmr: tauriHost
            ? {
                  protocol: 'ws',
                  host: tauriHost,
                  port: 5174,
              }
            : undefined,
        watch: {
            ignored: ['**/src-tauri/**'],
        },
    },
    envPrefix: ['VITE_', 'TAURI_ENV_*'],
    build: {
        target: process.env.TAURI_ENV_PLATFORM === 'windows' ? 'chrome105' : 'safari13',
        minify: process.env.TAURI_ENV_DEBUG ? false : 'oxc',
        sourcemap: Boolean(process.env.TAURI_ENV_DEBUG),
    },
});
