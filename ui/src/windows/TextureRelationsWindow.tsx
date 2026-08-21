import { useCallback, useEffect, useRef, useState } from "react";
import { invoke } from "../bridge/native";
import type { TextureDiagnosticPage } from "../bridge/contracts";

const pageSize = 100;

export function TextureRelationsWindow() {
  const [activeOnly, setActiveOnly] = useState(false);
  const [includeViews, setIncludeViews] = useState(true);
  const [page, setPage] = useState<TextureDiagnosticPage>();
  const [error, setError] = useState("");
  const [loading, setLoading] = useState(false);
  const requestSequence = useRef(0);

  const load = useCallback(async (offset: number, generation: string) => {
    const sequence = ++requestSequence.current;
    setLoading(true); setError("");
    try {
      const result = await invoke("diagnostics.getTextureRelations", { generation, offset, limit: pageSize, activeOnly, includeViews });
      if (sequence === requestSequence.current) setPage(result);
    }
    catch (reason) { if (sequence === requestSequence.current) setError(String(reason)); }
    finally { if (sequence === requestSequence.current) setLoading(false); }
  }, [activeOnly, includeViews]);

  useEffect(() => { void load(0, "0"); }, [load]);
  const offset = page?.offset ?? 0;
  return <main className="tool-window diagnostic-window">
    <header><div><div className="eyebrow">Graphics diagnostics</div><h1>Texture relations</h1></div><button onClick={() => void load(0, "0")} disabled={loading}>Refresh snapshot</button></header>
    <div className="toolbar embedded">
      <label className="check-row"><input type="checkbox" checked={activeOnly} onChange={event => setActiveOnly(event.target.checked)} /> Active only</label>
      <label className="check-row"><input type="checkbox" checked={includeViews} onChange={event => setIncludeViews(event.target.checked)} /> Include views</label>
      <span>{page ? `${page.total} copied rows · generation ${page.generation}` : "Loading…"}</span>
    </div>
    {error && <p className="error" role="alert">{error}</p>}
    {page?.diagnostic && <p className="warning">{page.diagnostic}</p>}
    {page?.truncated && <p className="warning">Snapshot was capped to protect the UI.</p>}
    <div className="diagnostic-table-wrap"><table className="diagnostic-table"><thead><tr><th>Kind</th><th>Dimensions</th><th>Format</th><th>Pitch</th><th>Slice range</th><th>Mip range</th><th>Age</th><th>Flags</th></tr></thead><tbody>
      {page?.rows.map(row => <tr key={row.id} className={row.kind === "view" ? "diagnostic-view" : ""}><td>{row.kind === "view" ? "↳ View" : `Texture${row.alternativeViewCount ? ` (${row.alternativeViewCount + 1})` : ""}`}</td><td>{row.kind === "view" ? row.dimension : `${row.effectiveWidth}×${row.effectiveHeight}${row.effectiveDepth > 1 ? `×${row.effectiveDepth}` : ""}`}</td><td>{row.format}{row.depthFormat ? " depth" : ""}</td><td>{row.pitch || "—"}</td><td>{row.sliceCount ? `${row.firstSlice}–${row.firstSlice + row.sliceCount - 1}` : "—"}</td><td>{row.mipCount ? `${row.firstMip}–${row.firstMip + row.mipCount - 1}` : "—"}</td><td>{row.kind === "texture" ? `${row.ageMilliseconds} ms` : "—"}</td><td>{[row.updatedOnGpu && "GPU", row.resolutionOverridden && "scaled", row.active && "active"].filter(Boolean).join(", ") || "—"}</td></tr>)}
    </tbody></table></div>
    <footer className="diagnostic-pager"><button disabled={loading || offset === 0} onClick={() => void load(Math.max(0, offset - pageSize), page!.generation)}>Previous</button><span>{page ? `${offset + 1}–${Math.min(page.total, offset + page.rows.length)} of ${page.total}` : "—"}</span><button disabled={loading || !page || offset + page.rows.length >= page.total} onClick={() => void load(offset + pageSize, page!.generation)}>Next</button></footer>
  </main>;
}
