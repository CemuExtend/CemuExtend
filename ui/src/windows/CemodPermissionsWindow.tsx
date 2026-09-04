import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type {
  Bootstrap,
  CemodManagerSnapshot,
  NativeEvent,
} from "../bridge/contracts";
import { subscribe } from "../bridge/events";
import { invoke } from "../bridge/native";
import { cemodEventJobId, parseCemodSnapshot } from "./cemodEvents";

type Context = NonNullable<Bootstrap["context"]>;

// The host serialises one entry per kCemodPermissions member
// (src/Cemu/CemuExtend/CemodInspectionService.h). Approval is blocked unless the
// package model carries the whole set, so this has to track that list: a short
// model means the dialog cannot show everything the package is asking for.
const CEMOD_PERMISSION_MODEL_SIZE = 18;

export function CemodPermissionsWindow({
  context,
}: {
  windowId: string;
  context?: Context;
}) {
  const [model, setModel] = useState<CemodManagerSnapshot>();
  const [grants, setGrants] = useState<Set<string>>(new Set());
  const [approved, setApproved] = useState(false);
  const [trustUpdates, setTrustUpdates] = useState(false);
  const [digestConfirmed, setDigestConfirmed] = useState(false);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState("");
  const activeJob = useRef<string | undefined>(undefined);
  const queuedEvents = useRef(new Map<string, NativeEvent>());

  const applyCompletion = useCallback(
    (event: NativeEvent, id: string) => {
      const completed = parseCemodSnapshot(event, id);
      if (!completed) return;
      activeJob.current = undefined;
      if (!completed.ok || completed.snapshot.cancelled) {
        setError(
          completed.diagnostic || `Inspection failed (${completed.error})`,
        );
        return;
      }
      if (completed.snapshot.generation !== context?.generation) {
        setError(
          "The package catalog changed. Close this dialog and review the refreshed package.",
        );
        return;
      }
      const next = completed.snapshot.packages.find(
        (candidate) => candidate.packageKey === context.packageKey,
      );
      if (!next) {
        setError("The exact package is no longer installed.");
        return;
      }
      setModel(completed.snapshot);
      setApproved(next.approved);
      setTrustUpdates(next.trustUpdates);
      setGrants(
        new Set(
          next.permissions
            .filter((permission) => permission.granted)
            .map((permission) => permission.bit),
        ),
      );
    },
    [context?.generation, context?.packageKey],
  );

  useEffect(() => {
    if (!context?.titleId || !context.packageKey || !context.generation) {
      setError("Approval context is incomplete.");
      return;
    }
    void invoke("cemod.discover", { titleId: context.titleId })
      .then(({ jobId: id }) => {
        activeJob.current = id;
        const queued = queuedEvents.current.get(id);
        queuedEvents.current.delete(id);
        if (queued) applyCompletion(queued, id);
      })
      .catch((reason: unknown) =>
        setError(reason instanceof Error ? reason.message : String(reason)),
      );
  }, [
    applyCompletion,
    context?.titleId,
    context?.packageKey,
    context?.generation,
  ]);
  useEffect(
    () =>
      subscribe((event) => {
        const eventJob = cemodEventJobId(event);
        if (!eventJob) return;
        if (eventJob !== activeJob.current) {
          queuedEvents.current.set(eventJob, event);
          return;
        }
        applyCompletion(event, eventJob);
      }),
    [applyCompletion],
  );
  const item = model?.packages.find(
    (candidate) => candidate.packageKey === context?.packageKey,
  );
  const selectedMask = useMemo(
    () => [...grants].reduce((mask, bit) => mask | BigInt(bit), 0n).toString(),
    [grants],
  );
  const permissionCountValid =
    item?.permissions.length === CEMOD_PERMISSION_MODEL_SIZE;
  const completeNativeGrant =
    !item ||
    (!item.trustedNative && !item.wups) ||
    (BigInt(selectedMask) & BigInt(item.requestedPermissions)) ===
      BigInt(item.requestedPermissions);

  async function save() {
    if (
      !item ||
      !context?.titleId ||
      !context.generation ||
      !digestConfirmed ||
      !permissionCountValid
    )
      return;
    setSaving(true);
    setError("");
    try {
      const result = await invoke("cemod.saveApproval", {
        generation: context.generation,
        titleId: context.titleId,
        packageKey: item.packageKey,
        grantedPermissions: selectedMask,
        approved,
        trustUpdates,
      });
      if (!result.ok) {
        setModel(result.snapshot);
        setError(result.diagnostic || `Approval failed (${result.error})`);
        return;
      }
      await invoke("window.close");
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
    } finally {
      setSaving(false);
    }
  }

  return (
    <main className="tool-window wizard-window cemod-permissions">
      <header>
        <div>
          <span className="eyebrow">Security boundary</span>
          <h1>Exact CemuMod approval</h1>
        </div>
      </header>
      {error && (
        <div className="error" role="alert">
          {error}
        </div>
      )}
      {!item && !error && (
        <div className="empty" aria-busy="true">
          Inspecting the exact installed package…
        </div>
      )}
      {item && (
        <section className="settings-section">
          {!item.runtimeAvailable && (
            <div className="warning">
              Runtime unavailable: saving this approval does not make the
              package executable in this build.
            </div>
          )}
          {item.headless && (
            <div className="warning">
              Headless package: it cannot present an interactive runtime UI.
            </div>
          )}
          {!permissionCountValid && (
            <div className="error">
              The host did not provide the complete{" "}
              {CEMOD_PERMISSION_MODEL_SIZE}-permission model. Approval is
              blocked.
            </div>
          )}
          <h2>{item.pluginName || item.modId}</h2>
          <p className="lead">
            Approve only this package digest. Replacing or changing the package
            invalidates this exact approval.
          </p>
          <div className="digest-card">
            <span>SHA-256 package digest</span>
            <code>{item.packageDigest}</code>
            <label className="check-row">
              <input
                type="checkbox"
                checked={digestConfirmed}
                onChange={(event) => setDigestConfirmed(event.target.checked)}
              />
              I verified this exact digest
            </label>
          </div>
          <h3 className="permission-heading">Requested permissions</h3>
          <div className="permission-list">
            {item.permissions.map((permission) => (
              <label
                key={permission.bit}
                className={`permission-row ${permission.dangerous ? "dangerous" : ""}`}
              >
                <input
                  type="checkbox"
                  disabled={!permission.requested || !item.valid}
                  checked={grants.has(permission.bit)}
                  onChange={(event) =>
                    setGrants((current) => {
                      const next = new Set(current);
                      if (event.target.checked) next.add(permission.bit);
                      else next.delete(permission.bit);
                      return next;
                    })
                  }
                />
                <span>
                  <strong>{permission.name}</strong>
                  <small>
                    {permission.requested
                      ? permission.dangerous
                        ? "Dangerous capability — grant only if you trust this exact package."
                        : "Requested by this package."
                      : "Not requested; cannot be granted."}
                    {permission.manifestMismatch
                      ? " Manifest and binary inspection disagree."
                      : ""}
                  </small>
                </span>
              </label>
            ))}
          </div>
          <label className="check-row approval-toggle">
            <input
              type="checkbox"
              checked={approved}
              disabled={!item.valid}
              onChange={(event) => setApproved(event.target.checked)}
            />
            Mark this exact package as approved
          </label>
          {approved && !completeNativeGrant && (
            <div className="error">
              Native and WUPS packages cannot run with only part of their
              requested permissions. Grant every requested permission or leave
              this package unapproved.
            </div>
          )}
          <label className="check-row approval-toggle">
            <input
              type="checkbox"
              checked={trustUpdates}
              disabled={!item.valid || !approved}
              onChange={(event) => setTrustUpdates(event.target.checked)}
            />
            Trust future updates of this mod
          </label>
          {trustUpdates && (
            <p className="lead">
              A rebuilt package is approved automatically as long as it asks for
              nothing beyond the permissions granted here. A version that wants
              more still comes back to this dialog.
            </p>
          )}
        </section>
      )}
      <footer className="actions">
        <button onClick={() => void invoke("window.close")}>Cancel</button>
        <button
          className="primary"
          disabled={
            !item ||
            !item.valid ||
            !digestConfirmed ||
            !permissionCountValid ||
            (approved && !completeNativeGrant) ||
            saving
          }
          onClick={() => void save()}
        >
          {saving ? "Saving…" : "Save exact decision"}
        </button>
      </footer>
    </main>
  );
}
