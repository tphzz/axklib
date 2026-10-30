import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { mkdir, writeFile } from 'node:fs/promises';
import { createServer as createSocketServer } from 'node:net';
import { resolve } from 'node:path';
import { createServer } from 'vite';

const output = resolve(process.argv[2]);
await mkdir(output, { recursive: true });
const server = await createServer({ server: { host: '127.0.0.1', port: 0, strictPort: false } });
const results = [];
let child, childExit, port, stopping;
function stop() {
    return stopping ??= (async () => {
        if (child?.pid) {
            try { process.kill(-child.pid, 'SIGTERM'); } catch (error) { if (error.code !== 'ESRCH') throw error; }
            const force = setTimeout(() => { try { process.kill(-child.pid, 'SIGKILL'); } catch {} }, 3000);
            try { await childExit; } finally { clearTimeout(force); }
        }
        await server.close();
    })();
}
const interrupted = () => { process.exitCode = 1; void stop(); };
process.once('SIGINT', interrupted); process.once('SIGTERM', interrupted);
const deadline = setTimeout(interrupted, 300000);
try {
    await server.listen();
    port = server.httpServer.address().port;
    const base = `http://127.0.0.1:${port}`;
    for (const [runner, name] of [
        ['sample-editor-layout.mjs', 'sample-layout'],
        ['sample-editor-stability.mjs', 'sample-stability'],
        ['storage-picker-layout.mjs', 'storage-picker'],
    ]) {
        const code = await new Promise((done, reject) => {
            child = spawn(process.execPath, [`tools/${runner}`, base, resolve(output, name)], { stdio: 'inherit', detached: true });
            childExit = new Promise(resolveExit => child.once('close', resolveExit));
            child.once('error', reject);
            child.once('close', code => { child = undefined; done(code); });
        });
        results.push({ runner, code });
        assert.equal(code, 0, runner);
    }
} finally {
    clearTimeout(deadline);
    await stop();
    process.removeListener('SIGINT', interrupted); process.removeListener('SIGTERM', interrupted);
    if (port) await new Promise((done, reject) => {
        const socket = createSocketServer(); socket.once('error', reject);
        socket.listen(port, '127.0.0.1', () => socket.close(error => error ? reject(error) : done()));
    });
    await writeFile(resolve(output, 'results.json'), JSON.stringify({ serverStopped: true, childrenStopped: !child, portReleased: !!port, results }, null, 2) + '\n');
}

// Vite plugins may retain worker handles after their browser and port have closed.
process.exit(process.exitCode ?? 0);
