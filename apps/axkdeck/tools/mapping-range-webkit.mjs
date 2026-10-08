import assert from 'node:assert/strict';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { createConnection, createServer as createSocketServer } from 'node:net';
import { resolve } from 'node:path';
import { spawn } from 'node:child_process';
import { createServer } from 'vite';

assert.ok(process.argv[2], 'Provide a build/reports output directory');
const output = resolve(process.argv[2]);
await mkdir(output, { recursive: true });
const port = await new Promise((resolve, reject) => {
    const socket = createSocketServer();
    socket.once('error', reject);
    socket.listen(0, '127.0.0.1', () => {
        const port = socket.address().port;
        socket.close(error => error ? reject(error) : resolve(port));
    });
});
const server = await createServer({ server: { host: '127.0.0.1', port, strictPort: true } });
let child, exited, stopping, outcomes;
async function stopChild() {
    if (!child || child.exitCode !== null || child.signalCode !== null) return;
    const kill = signal => { try { process.kill(-child.pid, signal); } catch (error) { if (error.code !== 'ESRCH') throw error; } };
    kill('SIGTERM');
    const force = setTimeout(() => kill('SIGKILL'), 5000);
    try { await exited; } finally { clearTimeout(force); }
}
const stop = () => stopping ??= (async () => { try { await stopChild(); } finally { await server.close(); } })();
const signal = () => { process.exitCode = 1; void stop(); };
process.once('SIGTERM', signal); process.once('SIGINT', signal);
const deadline = setTimeout(signal, 120000);
try {
    await server.listen();
    child = spawn('/usr/bin/python3', ['tools/sample-editor-webkit.py', `http://127.0.0.1:${port}`, output,
        '--width', '1040', '--height', '680', '--zoom', '1', '--zoom', '1.25', '--zoom', '1.5',
        '--fixture', '/tools/layout-fixtures/mapping-presentation.html?live',
        '--checks', 'tools/mapping-range-checks.js', '--checks-function', 'runMappingRangeChecks'],
        { stdio: 'inherit', detached: true });
    exited = new Promise((resolve, reject) => { child.once('error', reject); child.once('exit', resolve); });
    assert.equal(await exited, 0, 'Native WebKitGTK range rendering checks');
    outcomes = JSON.parse(await readFile(resolve(output, 'webkit-results.json'), 'utf8'));
    assert.equal(outcomes.length, 6, 'Complete native 100/125/150-percent matrix');
} finally {
    clearTimeout(deadline); process.removeListener('SIGTERM', signal); process.removeListener('SIGINT', signal);
    await stop();
    const portReleased = await new Promise(resolve => {
        const socket = createConnection({ host: '127.0.0.1', port });
        socket.once('connect', () => { socket.destroy(); resolve(false); });
        socket.once('error', error => { socket.destroy(); resolve(error.code === 'ECONNREFUSED'); });
        socket.setTimeout(2000, () => { socket.destroy(); resolve(false); });
    });
    await writeFile(resolve(output, 'results.json'), JSON.stringify({ port, portReleased, serverStopped: !server.httpServer?.listening, childStopped: child?.exitCode !== null || child?.signalCode !== null, outcomes }, null, 2) + '\n');
    assert.ok(portReleased, 'Owned WebKit test server port must be released');
}
