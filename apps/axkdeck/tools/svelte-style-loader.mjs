// A cached browser module can request its virtual stylesheet before the parent
// has been compiled in a fresh dev server. Populate Svelte's CSS metadata first.
export function svelteStyleLoader() {
    let server;
    return {
        name: 'axkdeck:svelte-style-loader',
        apply: 'serve',
        enforce: 'pre',
        configureServer(value) {
            server = value;
        },
        async load(id) {
            if (server && id.endsWith('?svelte&type=style&lang.css')) {
                await server.environments.client.transformRequest(id.slice(0, id.indexOf('?')));
            }
        },
    };
}
