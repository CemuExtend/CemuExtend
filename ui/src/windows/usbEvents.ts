import type { UsbDevicesChangedPayload, UsbDeviceDescriptor } from "../bridge/contracts";

function isUint(value: unknown, maximum: number): value is number {
  return typeof value === "number" && Number.isSafeInteger(value) && value >= 0 && value <= maximum;
}

function descriptor(value: unknown): UsbDeviceDescriptor | undefined {
  if (!value || typeof value !== "object") return undefined;
  const item = value as Record<string, unknown>;
  if (typeof item.id !== "string" || item.id.length === 0 || item.id.length > 64 ||
      !isUint(item.vendorId, 0xffff) || !isUint(item.productId, 0xffff) ||
      !isUint(item.interfaceIndex, 0xff) || !isUint(item.interfaceSubClass, 0xff) ||
      !isUint(item.protocol, 0xff) || !isUint(item.maxPacketSizeRx, 0xffff) ||
      !isUint(item.maxPacketSizeTx, 0xffff) || typeof item.opened !== "boolean") return undefined;
  return item as UsbDeviceDescriptor;
}

export function parseUsbDeviceChange(type: string, payload: unknown, currentGeneration: string): UsbDevicesChangedPayload | undefined {
  if (type !== "usb.devicesChanged" || !payload || typeof payload !== "object") return undefined;
  const item = payload as Record<string, unknown>;
  const device = descriptor(item.device);
  if (typeof item.generation !== "string" || !/^(0|[1-9][0-9]*)$/.test(item.generation) ||
      BigInt(item.generation) <= BigInt(currentGeneration) ||
      typeof item.attached !== "boolean" || !device) return undefined;
  return { generation: item.generation, attached: item.attached, device };
}
