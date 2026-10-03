// A cached browser module can request its virtual stylesheet before the parent
// has been compiled in a fresh dev server, even after its styles were removed.
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
                const componentId = id.slice(0, id.indexOf('?'));
                await server.environments.client.transformRequest(componentId);
                const resolved = await this.resolve(componentId);
                const svelte = resolved && this.getModuleInfo(resolved.id)?.meta.svelte;
                // Only a successfully compiled component with no CSS permits an empty response.
                if (svelte?.css === null) {
                    return { code: '', moduleType: 'css' };
                }
            }
        },
    };
}
