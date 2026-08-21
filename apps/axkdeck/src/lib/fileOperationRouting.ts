import type { ConnectionMode } from './transport';

export function shouldUseDirectComputerFileOperations(isDesktop: boolean, connectionMode: ConnectionMode): boolean {
    return isDesktop && connectionMode === 'local';
}
