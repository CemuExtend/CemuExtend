import { useCallback, useEffect, useRef, useState } from "react";
import type { EmulatedUsbModel } from "../bridge/contracts";
import { subscribe } from "../bridge/events";
import { invoke } from "../bridge/native";
import { parseUsbDeviceChange } from "./usbEvents";

const hex = (value: number) =>
  value.toString(16).padStart(4, "0").toUpperCase();

export function EmulatedUsbDevicesWindow() {
  const [model, setModel] = useState<EmulatedUsbModel>();
  const [busy, setBusy] = useState<string>();
  const [error, setError] = useState("");
  const generation = useRef("0");
  const install = useCallback((value: EmulatedUsbModel) => {
    if (BigInt(value.generation) < BigInt(generation.current)) return;
    generation.current = value.generation;
    setModel(value);
  }, []);
  const load = useCallback(
    async () => install(await invoke("usb.getModel")),
    [install],
  );

  useEffect(() => {
    void load().catch((reason: unknown) => setError(String(reason)));
    return subscribe((event) => {
      const change = parseUsbDeviceChange(
        event.type,
        event.payload,
        generation.current,
      );
      if (!change) return;
      generation.current = change.generation;
      void load().catch((reason: unknown) => setError(String(reason)));
    });
  }, [load]);

  async function toggle(
    device: EmulatedUsbModel["emulatedDevices"][number],
    enabled: boolean,
  ) {
    setBusy(device.id);
    setError("");
    try {
      install(
        await invoke("usb.setEnabled", {
          deviceId: device.id,
          vendorId: device.vendorId,
          productId: device.productId,
          enabled,
        }),
      );
    } catch (reason) {
      setError(String(reason));
      void load().catch(() => undefined);
    } finally {
      setBusy(undefined);
    }
  }

  return (
    <main className="role-window">
      <header>
        <div>
          <span className="eyebrow">Portal emulation</span>
          <h1>Emulated USB Devices</h1>
        </div>
        <div className="button-row">
          <button disabled={Boolean(busy)} onClick={() => void load()}>
            Refresh
          </button>
        </div>
      </header>
      <p className="lead">
        Enable the virtual portal, base, or toypad used by supported games.
        Changes are saved immediately and take effect when the HID backend is
        next initialized.
      </p>
      {error && (
        <div className="notice error" role="alert">
          {error}
        </div>
      )}
      {!model ? (
        <p>Loading USB devices…</p>
      ) : (
        <>
          <section className="usb-card-grid" aria-label="Emulated devices">
            {model.emulatedDevices.map((device) => (
              <article className="card" key={device.id}>
                <div className="pack-heading">
                  <div>
                    <h2>{device.name}</h2>
                    <code>
                      {hex(device.vendorId)}:{hex(device.productId)}
                    </code>
                  </div>
                  <span
                    className={device.connected ? "status-online" : "muted"}
                  >
                    {device.connected ? "Attached" : "Not attached"}
                  </span>
                </div>
                <label className="check-row">
                  <input
                    type="checkbox"
                    checked={device.enabled}
                    disabled={Boolean(busy)}
                    onChange={(event) =>
                      void toggle(device, event.target.checked)
                    }
                  />
                  Emulate this device
                </label>
              </article>
            ))}
          </section>
          <section className="editor-panel usb-attached">
            <h2>Attached HID snapshot</h2>
            <p className="muted">
              Only copied USB descriptor fields are shown; native handles and
              device pointers never leave the host.
            </p>
            {model.attachedDevices.length === 0 ? (
              <p>No supported HID devices are currently attached.</p>
            ) : (
              <div className="property-grid">
                {model.attachedDevices.map((device, index) => (
                  <div key={`${device.id}-${index}`}>
                    <strong>
                      {hex(device.vendorId)}:{hex(device.productId)}
                    </strong>
                    <span>
                      Interface {device.interfaceIndex} · protocol{" "}
                      {device.protocol} · RX/TX {device.maxPacketSizeRx}/
                      {device.maxPacketSizeTx} ·{" "}
                      {device.opened ? "open" : "closed"}
                    </span>
                  </div>
                ))}
              </div>
            )}
          </section>
        </>
      )}
    </main>
  );
}
