import { LogicalSize } from '@tauri-apps/api/dpi';
import { getCurrentWebview } from '@tauri-apps/api/webview';
import { currentMonitor, getCurrentWindow } from '@tauri-apps/api/window';

import type { InterfaceScaleAdapter } from './interfaceScale';

export function createTauriInterfaceScaleAdapter(): InterfaceScaleAdapter {
    const window = getCurrentWindow();
    const webview = getCurrentWebview();
    return {
        currentMonitor: async () => {
            const monitor = await currentMonitor();
            return monitor
                ? {
                      name: monitor.name,
                      physicalWidth: monitor.size.width,
                      physicalHeight: monitor.size.height,
                      scaleFactor: monitor.scaleFactor,
                  }
                : null;
        },
        windowScaleFactor: () => window.scaleFactor(),
        innerSize: async () => {
            const size = await window.innerSize();
            return { width: size.width, height: size.height };
        },
        setMinSize: (width, height) => window.setMinSize(new LogicalSize(width, height)),
        setSize: (width, height) => window.setSize(new LogicalSize(width, height)),
        setZoom: (zoom) => webview.setZoom(zoom),
        onMoved: async (handler) => window.onMoved(handler),
        onScaleChanged: async (handler) => window.onScaleChanged(handler),
    };
}
