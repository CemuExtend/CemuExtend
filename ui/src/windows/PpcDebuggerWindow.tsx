import { useCallback, useEffect, useRef, useState } from "react";
import type {
  GuestAddress,
  PpcDebuggerControl,
  PpcDebuggerSnapshot,
} from "../bridge/contracts";
import { invoke } from "../bridge/native";
import { translate } from "../i18n/runtime";
import { centerAddress, parseGuestAddress } from "./ppcDebuggerModel";

const LINE_COUNT = 64;

export function PpcDebuggerWindow() {
  const [center, setCenter] = useState<GuestAddress>("00000000");
  const [addressInput, setAddressInput] = useState("00000000");
  const [snapshot, setSnapshot] = useState<PpcDebuggerSnapshot>();
  const [error, setError] = useState("");
  const requestSequence = useRef(0);
  const refresh = useCallback(
    async (nextCenter: GuestAddress = center) => {
      const sequence = ++requestSequence.current;
      try {
        const next = await invoke("ppcDebugger.snapshot", {
          center: nextCenter,
          instructionCount: LINE_COUNT,
        });
        if (sequence !== requestSequence.current) return;
        setSnapshot(next);
        setError("");
        if (
          nextCenter === "00000000" &&
          next.instructionPointer !== "00000000"
        ) {
          setCenter(next.instructionPointer);
          setAddressInput(next.instructionPointer);
        }
      } catch (reason) {
        if (sequence === requestSequence.current) setError(String(reason));
      }
    },
    [center],
  );
  useEffect(() => {
    void refresh();
  }, [refresh]);

  const mutate = async (action: () => Promise<unknown>) => {
    const sequence = ++requestSequence.current;
    try {
      await action();
      if (sequence === requestSequence.current) await refresh();
    } catch (reason) {
      if (sequence !== requestSequence.current) return;
      setError(String(reason));
      await refresh();
    }
  };
  const control = (command: PpcDebuggerControl) =>
    snapshot &&
    void mutate(() =>
      invoke("ppcDebugger.control", {
        generation: snapshot.generation,
        command,
      }),
    );
  const go = () => {
    const parsed = parseGuestAddress(addressInput);
    if (!parsed || (Number.parseInt(parsed, 16) & 3) !== 0) {
      setError(translate("Enter a 4-byte aligned Wii U virtual address."));
      return;
    }
    setCenter(parsed);
    setAddressInput(parsed);
  };

  return (
    <main className="role-window ppc-debugger-window">
      <header>
        <div>
          <h1>PPC Debugger</h1>
          <p>Execution control, disassembly, registers, and breakpoints.</p>
        </div>
        <div className="button-row">
          <button onClick={() => void refresh()}>Refresh</button>
        </div>
      </header>
      {error && (
        <div className="notice error" role="alert">
          {error}
        </div>
      )}
      <section className="ppc-debugger-toolbar">
        <div className="button-row">
          <button
            disabled={!snapshot?.available || snapshot.trapped}
            onClick={() => control("break")}
          >
            Break
          </button>
          <button disabled={!snapshot?.trapped} onClick={() => control("run")}>
            Run
          </button>
          <button
            disabled={!snapshot?.trapped}
            onClick={() => control("stepInto")}
          >
            Step into
          </button>
          <button
            disabled={!snapshot?.trapped}
            onClick={() => control("stepOver")}
          >
            Step over
          </button>
        </div>
        <div className="button-row">
          <button
            onClick={() => {
              const next = centerAddress(center, -48);
              setCenter(next);
              setAddressInput(next);
            }}
          >
            ↑
          </button>
          <input
            aria-label="Guest address"
            value={addressInput}
            onChange={(event) => setAddressInput(event.target.value)}
            onKeyDown={(event) => {
              if (event.key === "Enter") go();
            }}
          />
          <button onClick={go}>Go</button>
          <button
            onClick={() => {
              const next = centerAddress(center, 48);
              setCenter(next);
              setAddressInput(next);
            }}
          >
            ↓
          </button>
        </div>
        <span className="status-pill">
          {snapshot?.trapped
            ? "Paused"
            : snapshot?.available
              ? "Running"
              : "Unavailable"}
        </span>
      </section>
      {!snapshot?.available && (
        <p className="muted">
          {snapshot?.diagnostic ||
            translate("Start a Wii U title to inspect PPC code.")}
        </p>
      )}
      <section className="ppc-debugger-grid">
        <div className="ppc-disassembly" aria-label="Disassembly">
          <div className="ppc-disassembly-row heading">
            <span>BP</span>
            <span>Address</span>
            <span>Opcode</span>
            <span>Instruction</span>
          </div>
          {snapshot?.instructions.map((instruction) => (
            <button
              className={`ppc-disassembly-row ${instruction.current ? "current" : ""} ${instruction.address === addressInput ? "selected" : ""}`}
              key={instruction.address}
              title={translate(
                "Select instruction; press Enter or double-click to toggle breakpoint",
              )}
              onClick={() => {
                setCenter(instruction.address);
                setAddressInput(instruction.address);
              }}
              onKeyDown={(event) => {
                if (event.key !== "Enter" || !snapshot) return;
                event.preventDefault();
                void mutate(() =>
                  invoke("ppcDebugger.toggleBreakpoint", {
                    generation: snapshot.generation,
                    address: instruction.address,
                  }),
                );
              }}
              onDoubleClick={() =>
                snapshot &&
                void mutate(() =>
                  invoke("ppcDebugger.toggleBreakpoint", {
                    generation: snapshot.generation,
                    address: instruction.address,
                  }),
                )
              }
            >
              <span>{instruction.breakpoint ? "●" : ""}</span>
              <code>{instruction.address}</code>
              <code>{instruction.opcode}</code>
              <code>
                <strong>{instruction.mnemonic}</strong> {instruction.operands}
              </code>
            </button>
          ))}
        </div>
        <aside className="ppc-debugger-side">
          <section>
            <h2>Registers</h2>
            <div className="ppc-registers">
              {snapshot?.gpr.map((value, index) => (
                <div key={index}>
                  <span>r{index}</span>
                  <code>{value}</code>
                </div>
              ))}
              <div>
                <span>lr</span>
                <code>{snapshot?.linkRegister}</code>
              </div>
            </div>
          </section>
          <section>
            <h2>Breakpoints</h2>
            {snapshot?.breakpointCapReached && (
              <p className="warning">
                Only the first 256 breakpoints are shown.
              </p>
            )}
            <div className="ppc-breakpoints">
              {snapshot?.breakpoints.map((breakpoint) => (
                <div key={breakpoint.identity}>
                  <label>
                    <input
                      type="checkbox"
                      checked={breakpoint.enabled}
                      onChange={(event) =>
                        void mutate(() =>
                          invoke("ppcDebugger.setBreakpointEnabled", {
                            generation: snapshot.generation,
                            identity: breakpoint.identity,
                            enabled: event.target.checked,
                          }),
                        )
                      }
                    />{" "}
                    <code>{breakpoint.address}</code>{" "}
                    {translate(breakpoint.logging ? "log" : "execute")}
                  </label>
                  <button
                    className="danger"
                    onClick={() =>
                      void mutate(() =>
                        invoke("ppcDebugger.deleteBreakpoint", {
                          generation: snapshot.generation,
                          identity: breakpoint.identity,
                        }),
                      )
                    }
                  >
                    Delete
                  </button>
                </div>
              ))}
              {snapshot?.breakpoints.length === 0 && (
                <p className="empty-results">No breakpoints.</p>
              )}
            </div>
          </section>
        </aside>
      </section>
    </main>
  );
}
