export async function prepareStartup<TConnection, TModule>(
    connection: Promise<TConnection>,
    waitForShellPaint: () => Promise<void>,
    loadModule: () => Promise<TModule>,
): Promise<{ connection: TConnection; module: TModule }> {
    await waitForShellPaint();
    const modulePromise = loadModule();
    const [connectionResult, moduleResult] = await Promise.all([connection, modulePromise]);
    return { connection: connectionResult, module: moduleResult };
}

export async function prepareServerConnection<TConnection>(
    restartLocal: boolean,
    useLocalServer: () => Promise<unknown>,
    loadConnection: () => Promise<TConnection>,
): Promise<TConnection> {
    if (restartLocal) await useLocalServer();
    return loadConnection();
}
