import type { DeviceCandidate } from '../lib/httpDevices';

export const deviceIdentity: DeviceCandidate['identity'] = {
    devicePath: '/dev/synthetic',
    usbManufacturer: 'Synthetic USB manufacturer',
    usbProduct: 'Synthetic USB reader',
    usbSerial: 'bridge-serial',
    usbVendorId: '1234',
    usbProductId: '5678',
};
