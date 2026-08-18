import { readFile, stat } from 'node:fs/promises';
import { fileURLToPath, pathToFileURL } from 'node:url';
import path from 'node:path';

const MAXIMUM_STARTUP_BYTES = 150 * 1024;

function localAssetPath(directory, asset) {
    return path.join(directory, asset.replace(/^\//, ''));
}

export async function checkStartupBundle(directory) {
    const indexPath = path.join(directory, 'index.html');
    const html = await readFile(indexPath, 'utf8');
    const entry = html.match(/<script[^>]+src="([^"]+)"/)?.[1];
    if (!entry) throw new Error(`No JavaScript entry was found in ${indexPath}`);
    const entryPath = localAssetPath(directory, entry);
    const entryBytes = (await stat(entryPath)).size;
    const preloads = [...html.matchAll(/<link[^>]+rel="modulepreload"[^>]+href="([^"]+)"/g)].map((match) => match[1]);
    const preloadBytes = await Promise.all(
        preloads.map(async (asset) => (await stat(localAssetPath(directory, asset))).size),
    );
    const startupBytes = entryBytes + preloadBytes.reduce((total, bytes) => total + bytes, 0);
    if (startupBytes > MAXIMUM_STARTUP_BYTES) {
        throw new Error(
            `Startup JavaScript is ${startupBytes} bytes; the ${MAXIMUM_STARTUP_BYTES}-byte budget requires the workspace to remain dynamically loaded.`,
        );
    }
    const source = await readFile(entryPath, 'utf8');
    if (!source.includes('App-') || !source.includes('import(')) {
        throw new Error('The generated startup entry does not dynamically load the workspace application.');
    }
    process.stdout.write(
        `Startup bundle check passed: ${startupBytes} bytes (${path.basename(entryPath)} plus ${preloads.length} preload${preloads.length === 1 ? '' : 's'})\n`,
    );
}

if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) {
    await checkStartupBundle(fileURLToPath(new URL('../dist', import.meta.url)));
}
