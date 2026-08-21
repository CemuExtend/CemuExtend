import { expect, test } from "bun:test";
import { parseUsbDeviceChange } from "./usbEvents";

const device = {
  id: "1430:0150:00",
  vendorId: 0x1430,
  productId: 0x0150,
  interfaceIndex: 0,
  interfaceSubClass: 0,
  protocol: 0,
  maxPacketSizeRx: 64,
  maxPacketSizeTx: 64,
  opened: false,
};

test("accepts only newer well-formed USB change events", () => {
  expect(
    parseUsbDeviceChange(
      "usb.devicesChanged",
      { generation: "4", attached: true, device },
      "3",
    )?.device.id,
  ).toBe(device.id);
  expect(
    parseUsbDeviceChange(
      "usb.devicesChanged",
      { generation: "3", attached: true, device },
      "3",
    ),
  ).toBeUndefined();
  expect(
    parseUsbDeviceChange(
      "usb.devicesChanged",
      {
        generation: "4",
        attached: true,
        device: { ...device, vendorId: 70000 },
      },
      "3",
    ),
  ).toBeUndefined();
  expect(
    parseUsbDeviceChange(
      "titles.changed",
      { generation: "4", attached: true, device },
      "3",
    ),
  ).toBeUndefined();
});
