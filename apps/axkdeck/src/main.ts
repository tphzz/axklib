import './app.css';
import { invoke } from '@tauri-apps/api/core';
import { mount, unmount } from 'svelte';
import StartupShell from './StartupShell.svelte';
import { installDiagnostics, reportDiagnostic, reportError, reportInfo } from './lib/diagnostics';
import {
    createInterfaceScaleController,
    type InterfaceScaleController,
    type InterfaceScaleMode,
} from './lib/interfaceScale';
import { prepareServerConnection, prepareStartup } from './lib/startupCoordinator';
import { frontendStartup, type StartupView } from './lib/startupDiagnostics';
import { revealAfterInterfaceScale, waitForVisibleScaleCommit } from './lib/startupVisibility';
import { createTauriInterfaceScaleAdapter, showCurrentTauriWindow } from './lib/tauriInterfaceScale';

type ServerConnection = NonNullable<Window['__AXKLIB_SERVER__']>;
type AppModule = typeof import('./App.svelte');

const target = document.getElementById('app');
if (!target) throw new Error('Unable to find the application mount point.');

async function connectServer(restartLocal: boolean): Promise<ServerConnection | null> {
    try {
        const connection = await prepareServerConnection(
            restartLocal,
            () => invoke('use_local_server'),
            async () => (await invoke<Window['__AXKLIB_SERVER__'] | null>('server_connection')) ?? null,
        );
        window.__AXKLIB_SERVER__ = connection ?? undefined;
        frontendStartup.markServerConnectionComplete();
        reportInfo(
            connection ? `Connected to ${connection.mode} axklib-server.` : 'No axklib-server connection is available.',
        );
        return connection;
    } catch (error) {
        window.__AXKLIB_SERVER__ = undefined;
        frontendStartup.markServerConnectionComplete();
        reportError('axklib-server is unavailable', error);
        return null;
    }
}

async function bootstrap(mountTarget: HTMLElement): Promise<void> {
    const isDesktop = '__TAURI_INTERNALS__' in window;
    let interfaceScaling: InterfaceScaleController | null = null;
    let warningOpen = true;
    let shellStatus: 'starting' | 'unavailable' = 'starting';
    let shellMessage = '';
    let shell: ReturnType<typeof mount> | null = null;
    const renderShell = () => {
        shell = mount(StartupShell, {
            target: mountTarget,
            props: {
                status: shellStatus,
                message: shellMessage,
                warningOpen,
                onacknowledge: () => {
                    warningOpen = false;
                    void replaceShell();
                },
                onretry: () => void startWorkspace(false, true),
                onopensettings: () => void startWorkspace(true),
            },
        });
    };
    const replaceShell = async () => {
        if (shell) await unmount(shell);
        renderShell();
    };
    renderShell();
    frontendStartup.markShellMounted();
    const diagnosticsReady = installDiagnostics().finally(() => frontendStartup.markDiagnosticsInstalled());
    const interfaceScalingReady = (async () => {
        if (!isDesktop) return;
        let initialMode: InterfaceScaleMode = 'auto';
        try {
            initialMode = await invoke<InterfaceScaleMode>('desktop_interface_scale_mode');
        } catch (error) {
            reportDiagnostic('interface_scale_preference_load_failed', { message: String(error) }, 'warn');
        }
        try {
            interfaceScaling = await createInterfaceScaleController(
                createTauriInterfaceScaleAdapter(),
                initialMode,
                (mode) => invoke('set_desktop_interface_scale_mode', { mode }),
                reportDiagnostic,
            );
        } catch (error) {
            reportDiagnostic('interface_scale_initialization_failed', { message: String(error) }, 'warn');
        }
    })().finally(() => frontendStartup.markInterfaceScaleComplete());
    const shellFirstFrame = revealAfterInterfaceScale(
        interfaceScalingReady,
        () => frontendStartup.waitForShellFirstFrame(),
        {
            showWindow: isDesktop ? showCurrentTauriWindow : undefined,
            waitForScaleCommit: isDesktop ? waitForVisibleScaleCommit : undefined,
        },
    );

    const view: StartupView =
        new URLSearchParams(window.location.search).get('view') === 'allocation' ? 'allocation' : 'workspace';
    if (view === 'allocation') {
        const connectionReady = isDesktop ? connectServer(false) : Promise.resolve(null);
        try {
            const moduleReady = shellFirstFrame.then(() =>
                import('./AllocationInspector.svelte').then((module) => {
                    frontendStartup.markAppModuleReady();
                    return module;
                }),
            );
            const [module, connection] = await Promise.all([
                moduleReady,
                connectionReady,
                diagnosticsReady,
                interfaceScalingReady,
            ]);
            if (isDesktop && !connection) {
                shellStatus = 'unavailable';
                shellMessage = 'Check the local service or configure a remote axklib-server connection.';
                await replaceShell();
                return;
            }
            if (shell) await unmount(shell);
            mount(module.default, { target: mountTarget });
            frontendStartup.markAppMounted();
        } catch (error) {
            shellStatus = 'unavailable';
            shellMessage = `The allocation inspector could not be loaded: ${String(error)}`;
            await replaceShell();
        }
        return;
    }

    async function startWorkspace(openSettings: boolean, restartLocal = false): Promise<void> {
        const refreshShell = shellStatus !== 'starting' || shellMessage.length > 0;
        shellStatus = 'starting';
        shellMessage = '';
        if (refreshShell) await replaceShell();
        const connectionPromise = isDesktop ? connectServer(restartLocal) : Promise.resolve(null);
        let prepared: { connection: ServerConnection | null; module: AppModule };
        try {
            const workspaceReady = prepareStartup(
                connectionPromise,
                () => shellFirstFrame,
                () =>
                    import('./App.svelte').then((module) => {
                        frontendStartup.markAppModuleReady();
                        return module;
                    }),
            );
            [prepared] = await Promise.all([workspaceReady, diagnosticsReady, interfaceScalingReady]);
        } catch (error) {
            shellStatus = 'unavailable';
            shellMessage = `The application workspace could not be loaded: ${String(error)}`;
            await replaceShell();
            return;
        }
        if (isDesktop && !prepared.connection && !openSettings) {
            shellStatus = 'unavailable';
            shellMessage = 'Check the local service or configure a remote axklib-server connection.';
            await replaceShell();
            return;
        }
        if (shell) await unmount(shell);
        mount(prepared.module.default, {
            target: mountTarget,
            props: {
                interfaceScaling,
                initialExperimentalWarningOpen: warningOpen,
                openConnectionSettingsOnStart: openSettings,
            },
        });
        frontendStartup.markAppMounted();
        if (isDesktop) {
            void frontendStartup.reportAfterAppFirstFrame('workspace').catch((error) => {
                reportDiagnostic('desktop_startup_report_failed', { message: String(error) }, 'warn');
            });
        }
    }

    await startWorkspace(false);
}

void bootstrap(target);
