import type { ReactNode } from "react";
import { CemuIcon, type CemuIconName } from "../components/CemuIcon";

function Group({ title, children }: { title: string; children: ReactNode }) {
  return (
    <section className="editor-panel settings-group">
      <h2>{title}</h2>
      <div className="settings-group__body">{children}</div>
    </section>
  );
}

function FieldRow({ label, children }: { label: string; children: ReactNode }) {
  return (
    <div className="row settings-field-row">
      <label>{label}</label>
      <div>{children}</div>
    </div>
  );
}

function Notice({
  tone = "info",
  children,
}: {
  tone?: "info" | "success" | "warning" | "error";
  children: ReactNode;
}) {
  const icon: CemuIconName =
    tone === "success"
      ? "check"
      : tone === "warning" || tone === "error"
        ? "warning"
        : "help";
  return (
    <div className={`notice ${tone}`}>
      <CemuIcon name={icon} />
      <span>{children}</span>
    </div>
  );
}

const Select = ({
  children,
  value,
}: {
  children?: ReactNode;
  value?: string;
}) => (
  <select className="field" defaultValue={value}>
    {children ?? <option>{value}</option>}
  </select>
);

const Check = ({
  children,
  checked = false,
}: {
  children: ReactNode;
  checked?: boolean;
}) => (
  <label className="check-row">
    <input type="checkbox" defaultChecked={checked} />
    <span>{children}</span>
  </label>
);

function GraphicsSettings() {
  return (
    <>
      <Notice>
        Changing the graphics API requires restarting the running title.
      </Notice>
      <Group title="Graphics API">
        <FieldRow label="API">
          <Select value="Vulkan" />
        </FieldRow>
        <FieldRow label="Graphics device">
          <Select value="NVIDIA GeForce RTX 4070" />
        </FieldRow>
        <FieldRow label="VSync">
          <Select value="Match emulated display" />
        </FieldRow>
      </Group>
      <Group title="Shader compilation">
        <Check checked>Async shader compilation</Check>
        <Check>Accurate GX2DrawDone synchronization</Check>
        <Check>Force mesh shaders</Check>
      </Group>
      <Group title="Scaling">
        <FieldRow label="Upscale filter">
          <Select value="Bicubic" />
        </FieldRow>
        <FieldRow label="Downscale filter">
          <Select value="Bilinear" />
        </FieldRow>
        <FieldRow label="Fullscreen scaling">
          <Select value="Keep aspect ratio" />
        </FieldRow>
      </Group>
    </>
  );
}

function AudioSettings() {
  return (
    <>
      <Group title="Audio backend">
        <FieldRow label="API">
          <Select value="Cubeb" />
        </FieldRow>
        <FieldRow label="Latency">
          <input type="range" defaultValue="80" />
          <span className="muted"> 80 ms</span>
        </FieldRow>
      </Group>
      <div className="three-col settings-audio-columns">
        {[
          ["TV output", "Default speakers", "Stereo", "90"],
          ["GamePad output", "Headphones", "Stereo", "70"],
          ["Input / Portal", "Microphone array", "Mono", "80"],
        ].map(([title, device, channels, volume]) => (
          <Group title={title} key={title}>
            <FieldRow label="Device">
              <Select value={device} />
            </FieldRow>
            <FieldRow label="Channels">
              <Select value={channels} />
            </FieldRow>
            <FieldRow label="Volume">
              <input type="range" defaultValue={volume} />
            </FieldRow>
          </Group>
        ))}
      </div>
    </>
  );
}

function OverlaySettings() {
  return (
    <div className="two-col">
      <Group title="Performance overlay">
        <FieldRow label="Position">
          <Select value="Top left" />
        </FieldRow>
        <FieldRow label="Scale">
          <Select value="100%" />
        </FieldRow>
        {[
          "FPS",
          "Draw calls",
          "CPU usage",
          "Per-core CPU usage",
          "RAM usage",
          "VRAM usage",
          "Debug lines",
        ].map((label, index) => (
          <Check checked={index !== 3 && index !== 6} key={label}>
            {label}
          </Check>
        ))}
      </Group>
      <Group title="Notifications">
        <FieldRow label="Position">
          <Select value="Top right" />
        </FieldRow>
        <FieldRow label="Scale">
          <Select value="100%" />
        </FieldRow>
        {[
          "Controller profile name",
          "Low controller battery",
          "Shader compilation",
          "Friend online",
          "Screenshot saved",
        ].map((label) => (
          <Check checked key={label}>
            {label}
          </Check>
        ))}
      </Group>
      <div className="runtime-preview-card">
        <dl>
          <div>
            <dt>FPS</dt>
            <dd>60.00</dd>
          </div>
          <div>
            <dt>CPU</dt>
            <dd>26.42%</dd>
          </div>
          <div>
            <dt>RAM</dt>
            <dd>4812 MB</dd>
          </div>
        </dl>
        <span>Controller profile loaded</span>
      </div>
    </div>
  );
}

function DebugSettings() {
  return (
    <>
      <Notice tone="warning">
        Developer options may reduce performance or produce large log files.
      </Notice>
      <Group title="Crash and diagnostics">
        <FieldRow label="Crash dump">
          <Select value="Mini dump" />
        </FieldRow>
        <FieldRow label="GDB stub port">
          <input className="field mono" defaultValue="1337" />
        </FieldRow>
        <Check>Enable GDB stub on launch</Check>
        <Check>Log memory breakpoints</Check>
      </Group>
      <Group title="GPU diagnostics">
        <Check>Enable GPU capture controls</Check>
        <FieldRow label="Capture directory">
          <input
            className="field mono wide-field"
            defaultValue="D:\CemuExtend\captures"
          />
        </FieldRow>
        <Check>Accurate Vulkan barriers</Check>
        <Check>Render upside down</Check>
      </Group>
      <Group title="Data dumps">
        <div className="four-checks">
          {[
            "Dump textures",
            "Dump shaders",
            "Dump recompiler functions",
            "Dump CURL requests",
          ].map((label) => (
            <Check key={label}>{label}</Check>
          ))}
        </div>
      </Group>
    </>
  );
}

function TcpGeckoSettings() {
  return (
    <>
      <Notice tone="warning">
        TCPGecko exposes memory access to external clients. Enable LAN access
        only on trusted networks.
      </Notice>
      <Group title="TCPGecko server">
        <Check checked>Enable TCPGecko</Check>
        <FieldRow label="Port">
          <input className="field mono" defaultValue="7331" />
        </FieldRow>
        <Check>Allow connections from the local network</Check>
        <FieldRow label="Handler version">
          <Select value="CemuExtend v2" />
        </FieldRow>
      </Group>
      <Group title="Connection status">
        <div className="property-grid">
          <div>
            <span>State</span>
            <strong>
              <b className="badge green">Listening</b>
            </strong>
          </div>
          <div>
            <span>Bind address</span>
            <strong className="mono">127.0.0.1:7331</strong>
          </div>
          <div>
            <span>Connected clients</span>
            <strong>0</strong>
          </div>
        </div>
      </Group>
    </>
  );
}

function ApplicationUpdate() {
  return (
    <>
      <Notice tone="success">
        CemuExtend 0.10.0 is available. You are running 0.9.0.
      </Notice>
      <Group title="Release information">
        <div className="property-grid">
          {[
            ["Current version", "0.9.0"],
            ["Available version", "0.10.0"],
            ["Channel", "Stable"],
            ["Package", "CemuExtend-win-x64.zip · 84.2 MB"],
          ].map(([a, b]) => (
            <div key={a}>
              <span>{a}</span>
              <strong>{b}</strong>
            </div>
          ))}
        </div>
      </Group>
      <Group title="Changes">
        <ul>
          <li>CemuExtend launcher interface</li>
          <li>Expanded per-game workspace</li>
          <li>Improved CemuMod permission review</li>
          <li>Runtime overlay keyboard and notifications</li>
        </ul>
      </Group>
      <div className="operation-progress">
        <div className="space-between">
          <span>Update package</span>
          <strong>0%</strong>
        </div>
        <progress value="0" max="100" />
        <small className="muted">Ready to download</small>
      </div>
    </>
  );
}

function AccountPreview({ index }: { index: number }) {
  return (
    <>
      <div className="split-view account-preview">
        <aside className="selection-list">
          <button className="selected">
            <strong>umi</strong>
            <code>80000001</code>
            <span>Active · Online ready</span>
          </button>
          <button>
            <strong>Guest</strong>
            <code>80000002</code>
            <span>Local account</span>
          </button>
          <button>
            <strong>PerformanceTest</strong>
            <code>80000003</code>
            <span>Local account</span>
          </button>
        </aside>
        <section className="editor-panel">
          <div className="pack-heading">
            <h2>Account identity</h2>
            <span className="badge green">Active</span>
          </div>
          <div className="property-grid">
            <div>
              <span>Mii name</span>
              <strong>umi</strong>
            </div>
            <div>
              <span>Persistent ID</span>
              <strong className="mono">80000001</strong>
            </div>
            <div>
              <span>Network</span>
              <strong>Pretendo Network</strong>
            </div>
          </div>
          <Group title="Network service">
            <FieldRow label="Service">
              <Select value="Pretendo Network" />
            </FieldRow>
            <div className="button-row mt10">
              <button className="button-primary">Set active account</button>
              <button>Open account folder</button>
            </div>
          </Group>
          <Group title="Actions">
            <div className="button-row">
              <button>Edit account</button>
              <button>Create account</button>
              <button className="danger">Delete account</button>
            </div>
          </Group>
        </section>
      </div>
      {index === 31 && (
        <StaticDialog
          title="Create local account"
          action="Create account"
          className="account-preview-dialog"
        >
          <FieldRow label="Account name">
            <input className="field wide-field" defaultValue="New Player" />
          </FieldRow>
          <FieldRow label="Persistent ID">
            <input className="field mono wide-field" defaultValue="80000004" />
          </FieldRow>
          <FieldRow label="Network service">
            <Select value="Offline" />
          </FieldRow>
          <Check checked>Set as active account after creation</Check>
          <Notice>
            Account data will be created in mlc01/usr/save/system/act.
          </Notice>
        </StaticDialog>
      )}
      {index === 32 && (
        <StaticDialog title="Delete account" action="Delete account">
          <Notice tone="error">
            Deleting an account does not automatically remove its save data.
            This action cannot be undone.
          </Notice>
          <div className="property-grid">
            <div>
              <span>Account</span>
              <strong>PerformanceTest</strong>
            </div>
            <div>
              <span>Persistent ID</span>
              <strong className="mono">80000003</strong>
            </div>
            <div>
              <span>Save directories</span>
              <strong>14 titles</strong>
            </div>
          </div>
          <Check>Also remove local account metadata</Check>
        </StaticDialog>
      )}
    </>
  );
}

const catalogRows = [
  ["Open Air Quest Update", "Update", "v208", "2.71 GB", "Available"],
  ["Open Air Quest DLC", "DLC", "v80", "1.42 GB", "Installed"],
  ["Kart Party U Update", "Update", "v64", "1.14 GB", "Available"],
  ["Splatter Arena Update", "Update", "v304", "612 MB", "Available"],
  ["Builder Blocks DLC", "DLC", "v32", "480 MB", "Not installed"],
];

function Catalog() {
  return (
    <>
      <Notice tone="success">
        Connected as umi · Pretendo Network. Catalog data is cached for this
        session.
      </Notice>
      <div className="controls">
        <label className="control grow">
          <span>Filter catalog</span>
          <input className="field" placeholder="Title or title ID" />
        </label>
        <label className="control">
          <span>Show</span>
          <Select value="All content" />
        </label>
        <button>
          <CemuIcon name="refresh" />
          Reconnect
        </button>
      </div>
      <PreviewTable
        headings={["Content", "Type", "Version", "Size", "Status"]}
        rows={catalogRows}
      />
      <Group title="Download selection">
        <div className="property-grid">
          <div>
            <span>Title ID</span>
            <strong className="mono">0005000E-101C9400</strong>
          </div>
          <div>
            <span>Target</span>
            <strong className="mono">mlc01/usr/title/0005000e/101c9400</strong>
          </div>
          <div>
            <span>Account</span>
            <strong>umi (80000001)</strong>
          </div>
        </div>
        <div className="button-row mt10">
          <button className="button-primary">
            <CemuIcon name="download" />
            Add to queue
          </button>
          <button>View installed content</button>
        </div>
      </Group>
    </>
  );
}

const titleRows = [
  ["Open Air Quest", "Base game", "v208", "26.4 GB", "Verified"],
  ["Open Air Quest", "Update", "v208", "2.71 GB", "Verified"],
  ["Open Air Quest", "DLC", "v80", "1.42 GB", "Verified"],
  ["Kart Party U", "Base game", "v64", "8.2 GB", "Verified"],
  ["Splatter Arena", "Base game", "v304", "6.8 GB", "Verified"],
  ["System Menu", "System", "v33", "412 MB", "Protected"],
];

function TitleManagerPreview({ index }: { index: number }) {
  return (
    <>
      <div className="controls">
        <label className="control grow">
          <span>Filter titles</span>
          <input className="field" placeholder="Name, title ID, or path" />
        </label>
        <label className="control">
          <span>Type</span>
          <Select value="All content" />
        </label>
        <button>
          <CemuIcon name="refresh" />
          Refresh
        </button>
        <button className="button-primary">Install title</button>
      </div>
      <PreviewTable
        headings={["Title", "Type", "Version", "Size", "Status"]}
        rows={titleRows}
      />
      <Group title="Selected title">
        <div className="property-grid title-properties">
          <div>
            <span>Title ID</span>
            <strong className="mono">00050000-101C9400</strong>
          </div>
          <div>
            <span>Region</span>
            <strong>USA</strong>
          </div>
          <div>
            <span>Format</span>
            <strong>Extracted</strong>
          </div>
          <div>
            <span>Path</span>
            <strong className="mono">D:\Games\Open Air Quest</strong>
          </div>
        </div>
        <div className="button-row mt10">
          <button className="button-primary">Launch</button>
          <button>Verify</button>
          <button>Convert to WUA</button>
          <button>Open folder</button>
          <button className="danger">Delete content</button>
        </div>
      </Group>
      {index === 41 && (
        <StaticDialog
          title="Install title content"
          action="Run in background"
          className="title-install-dialog"
          actionFirst
          actionEmphasis={false}
        >
          <p>Installing Open Air Quest Update v208…</p>
          <div className="operation-progress">
            <div className="space-between">
              <span>Copying content files</span>
              <strong>71%</strong>
            </div>
            <progress value="71" max="100" />
            <small className="muted">1.92 GB / 2.71 GB · 76 MB/s</small>
          </div>
          <div className="operation-progress">
            <div className="space-between">
              <span>Verifying written files</span>
              <strong>32%</strong>
            </div>
            <progress value="32" max="100" />
            <small className="muted">4,218 / 13,090 files</small>
          </div>
          <Notice>
            The game library will refresh automatically after installation.
          </Notice>
        </StaticDialog>
      )}
      {index === 42 && (
        <StaticDialog title="Convert title to WUA" action="Convert">
          <FieldRow label="Source">
            <input
              className="field wide-field"
              defaultValue="D:\Games\Open Air Quest"
            />
          </FieldRow>
          <FieldRow label="Destination">
            <input
              className="field wide-field"
              defaultValue="D:\Archives\OpenAirQuest.wua"
            />
          </FieldRow>
          <Check checked>Include update and DLC content</Check>
          <Check checked>Verify archive after conversion</Check>
          <Notice>Free space: 92 GB · estimated archive size: 23.8 GB</Notice>
        </StaticDialog>
      )}
      {index === 43 && (
        <StaticDialog title="Delete title content" action="Delete permanently">
          <Notice tone="error">
            The selected title content will be permanently removed from storage.
            Save data is not included.
          </Notice>
          <div className="property-grid">
            <div>
              <span>Title</span>
              <strong>Open Air Quest Update</strong>
            </div>
            <div>
              <span>Path</span>
              <strong className="mono">
                D:\CemuExtend\mlc01\usr\title\0005000e\101c9400
              </strong>
            </div>
            <div>
              <span>Size</span>
              <strong>2.71 GB</strong>
            </div>
          </div>
          <Check>I understand content cannot be recovered</Check>
        </StaticDialog>
      )}
    </>
  );
}

const saveRows = [
  ["Open Air Quest", "umi", "80000001", "86.4 MB", "2026-08-22 21:14"],
  ["Open Air Quest", "Guest", "80000002", "54.1 MB", "2026-08-12 08:02"],
  ["Kart Party U", "umi", "80000001", "12.7 MB", "2026-08-20 18:42"],
  ["Splatter Arena", "umi", "80000001", "34.2 MB", "2026-08-19 16:33"],
  ["Builder Blocks", "Guest", "80000002", "118 MB", "2026-08-15 10:04"],
];

function SaveManagerPreview({ index }: { index: number }) {
  return (
    <>
      <div className="controls">
        <label className="control grow">
          <span>Filter save data</span>
          <input className="field" placeholder="Title or account" />
        </label>
        <label className="control">
          <span>Account</span>
          <Select value="All accounts" />
        </label>
        <button>
          <CemuIcon name="refresh" />
          Refresh
        </button>
      </div>
      <PreviewTable
        headings={["Title", "Account", "Persistent ID", "Size", "Modified"]}
        rows={saveRows}
      />
      <Group title="Selected save">
        <div className="property-grid">
          <div>
            <span>Title ID</span>
            <strong className="mono">00050000-101C9400</strong>
          </div>
          <div>
            <span>Save path</span>
            <strong className="mono">
              mlc01/usr/save/00050000/101c9400/user/80000001
            </strong>
          </div>
          <div>
            <span>Owner</span>
            <strong>umi · 80000001</strong>
          </div>
        </div>
        <div className="button-row mt10">
          <button>Open folder</button>
          <button className="button-primary">Export</button>
          <button>Import</button>
          <button>Transfer</button>
          <button className="danger">Delete</button>
        </div>
      </Group>
      {index === 45 && (
        <StaticDialog title="Import save data" action="Import">
          <FieldRow label="Source archive">
            <input
              className="field wide-field"
              defaultValue="D:\Backups\OpenAirQuest_Save.zip"
            />
          </FieldRow>
          <FieldRow label="Target account">
            <Select value="umi (80000001)" />
          </FieldRow>
          <Check checked>
            Create a backup before overwriting existing data
          </Check>
          <Notice tone="success">
            The imported archive contains save data for Open Air Quest.
          </Notice>
        </StaticDialog>
      )}
      {index === 46 && (
        <StaticDialog title="Transfer save data" action="Transfer">
          <FieldRow label="Title">
            <strong>Open Air Quest</strong>
          </FieldRow>
          <FieldRow label="Source account">
            <Select value="umi (80000001)" />
          </FieldRow>
          <FieldRow label="Target account">
            <Select value="Guest (80000002)" />
          </FieldRow>
          <Check checked>Keep a copy under the source account</Check>
          <Check>Overwrite an existing destination save</Check>
        </StaticDialog>
      )}
      {index === 47 && (
        <StaticDialog title="Delete save data" action="Delete save">
          <Notice tone="error">
            This save directory will be permanently removed. Export a backup
            first if needed.
          </Notice>
          <div className="property-grid">
            <div>
              <span>Title</span>
              <strong>Open Air Quest</strong>
            </div>
            <div>
              <span>Account</span>
              <strong>umi · 80000001</strong>
            </div>
            <div>
              <span>Size</span>
              <strong>86.4 MB</strong>
            </div>
          </div>
          <Check>I understand this save cannot be recovered</Check>
        </StaticDialog>
      )}
    </>
  );
}

function DownloadsPreview() {
  return (
    <>
      <Notice>
        Three jobs are active. Closing this window does not cancel background
        work.
      </Notice>
      <PreviewTable
        headings={["Job", "Type", "Status", "Progress", "Transferred"]}
        rows={[
          [
            "Open Air Quest Update",
            "Title install",
            "Running",
            "68%",
            "1.84 / 2.71 GB",
          ],
          [
            "Community graphic packs",
            "Package update",
            "Extracting",
            "42%",
            "384 / 912 MB",
          ],
          ["Open Air Quest DLC", "Download", "Queued", "0%", "0 / 1.42 GB"],
          ["Application update", "CemuExtend", "Complete", "100%", "84.2 MB"],
        ]}
      />
      <Group title="Selected job">
        <div className="operation-progress">
          <div className="space-between">
            <strong>Open Air Quest Update</strong>
            <span>68%</span>
          </div>
          <progress value="68" max="100" />
          <small className="muted">Copying content\Pack\Dungeon102.pack</small>
        </div>
        <div className="button-row">
          <button>Pause</button>
          <button className="danger">Cancel</button>
          <button>Open destination</button>
        </div>
      </Group>
    </>
  );
}

function PreviewTable({
  headings,
  rows,
}: {
  headings: string[];
  rows: string[][];
}) {
  return (
    <div className="table-wrap">
      <table>
        <thead>
          <tr>
            {headings.map((h) => (
              <th key={h}>{h}</th>
            ))}
          </tr>
        </thead>
        <tbody>
          {rows.map((row, i) => (
            <tr
              className={i === 0 ? "selected" : undefined}
              key={row.join("-")}
            >
              {row.map((cell, j) => (
                <td
                  className={cell.startsWith("0x") ? "mono" : undefined}
                  key={`${j}-${cell}`}
                >
                  {cell}
                </td>
              ))}
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

function DebuggerPreview({ index }: { index: number }) {
  if (index === 55)
    return (
      <>
        <Notice>Register snapshot captured at breakpoint 0x0201A900.</Notice>
        <div className="register-grid">
          {Array.from({ length: 32 }, (_, i) => (
            <div key={i}>
              <b>r{i}</b>
              <span>
                {(0x10000000 + i * 0x10a4).toString(16).toUpperCase()}
              </span>
            </div>
          ))}
        </div>
        <Group title="Special registers">
          <div className="register-grid special-registers">
            {[
              ["LR", "0201A90C"],
              ["CTR", "0000003C"],
              ["CR", "24000422"],
              ["XER", "00000000"],
              ["MSR", "00000000"],
              ["CIA", "0201A900"],
              ["FPSCR", "00000000"],
              ["TB", "88149F20"],
            ].map(([r, v]) => (
              <div key={r}>
                <b>{r}</b>
                <span>{v}</span>
              </div>
            ))}
          </div>
        </Group>
      </>
    );
  if (index === 56)
    return (
      <>
        <div className="controls">
          <label className="control grow">
            <span>Address</span>
            <input className="field mono" defaultValue="0x1024A000" />
          </label>
          <label className="control">
            <span>Length</span>
            <Select value="4 KB" />
          </label>
          <button className="button-primary">Go</button>
          <button>Save dump</button>
        </div>
        <pre className="hex-dump">
          {Array.from(
            { length: 25 },
            (_, i) =>
              `${(0x1024a000 + i * 16).toString(16).toUpperCase()}  ${Array.from({ length: 16 }, (_, j) => ((i * 13 + j * 7) % 256).toString(16).padStart(2, "0").toUpperCase()).join(" ")}  |${"ABCDEFGHIJKLMNOPQRSTUVWXYZ".slice(i % 12, (i % 12) + 16).padEnd(16, "A")}|`,
          ).join("\n")}
        </pre>
      </>
    );
  if (index === 57)
    return (
      <>
        <div className="button-row preview-action-row">
          <button className="button-primary">+ Add breakpoint</button>
          <button>Remove</button>
          <button>Enable all</button>
          <button>Disable all</button>
        </div>
        <PreviewTable
          headings={["State", "Type", "Address", "Symbol / Note", "Hits"]}
          rows={[
            ["Enabled", "Execution", "0x0201A900", "GameLoop::Tick", "Hit 4"],
            ["Enabled", "Execution", "0x0201A908", "FrameLimiter", "Hit 0"],
            ["Disabled", "Memory write", "0x1024A0B8", "Frame time", "Hit 12"],
            [
              "Enabled",
              "Memory read",
              "0x10550010",
              "Player position",
              "Hit 1",
            ],
          ]}
        />
        <Group title="Breakpoint properties">
          <FieldRow label="Condition">
            <input className="field mono wide-field" defaultValue="r4 == 0" />
          </FieldRow>
          <FieldRow label="Action">
            <Select value="Pause execution" />
          </FieldRow>
          <Check>Remove after first hit</Check>
        </Group>
      </>
    );
  if (index === 58)
    return (
      <>
        <PreviewTable
          headings={["Module", "Base", "Size", "Description", "State"]}
          rows={[
            [
              "main.rpx",
              "0x02000000",
              "0x009A4000",
              "Open Air Quest executable",
              "Loaded",
            ],
            [
              "nn_act.rpl",
              "0x10000000",
              "0x0008A000",
              "Account library",
              "Loaded",
            ],
            ["gx2.rpl", "0x10100000", "0x00134000", "Graphics API", "Loaded"],
            [
              "snd_core.rpl",
              "0x10300000",
              "0x0007C000",
              "Audio core",
              "Loaded",
            ],
            [
              "cemod_framepacer.wasm",
              "0x18000000",
              "0x00012000",
              "CemuMod module",
              "Loaded",
            ],
          ]}
        />
        <Group title="Module map">
          <div className="button-row">
            <button className="button-primary">Load map file</button>
            <button>Unload symbols</button>
            <button>Open module memory</button>
          </div>
          <p className="muted">
            Map files can add symbol names to disassembly and breakpoint views.
          </p>
        </Group>
      </>
    );
  return (
    <>
      <div className="controls">
        <label className="control grow">
          <span>Filter symbols</span>
          <input className="field" placeholder="Name or address" />
        </label>
        <label className="control">
          <span>Module</span>
          <Select value="All modules" />
        </label>
        <button>Load map</button>
      </div>
      <PreviewTable
        headings={["Address", "Symbol", "Module", "Kind"]}
        rows={[
          ["0x0201A900", "GameLoop::Tick", "main.rpx", "Function"],
          ["0x0201B4C0", "Renderer::Present", "main.rpx", "Function"],
          ["0x0204D120", "Player::Update", "main.rpx", "Function"],
          ["0x0214C220", "FrameLimiter::Wait", "main.rpx", "Function"],
          ["0x1012A430", "GX2DrawDone", "gx2.rpl", "Export"],
          ["0x10308810", "AXVoiceBegin", "snd_core.rpl", "Export"],
        ]}
      />
      <div className="button-row mt10">
        <button className="button-primary">Go to disassembly</button>
        <button>Add breakpoint</button>
        <button>Copy name</button>
      </div>
    </>
  );
}

const slotNames = [
  "Slot 1",
  "Slot 2",
  "Slot 3",
  "Slot 4",
  "Slot 5",
  "Slot 6",
  "Slot 7",
  "Slot 8",
];
function FigureSlots({ disney = false }: { disney?: boolean }) {
  const names = disney
    ? [
        "Hexagonal slot",
        "Player 1",
        "Player 2",
        "Power Disc 1",
        "Power Disc 2",
        "Power Disc 3",
      ]
    : slotNames;
  return (
    <div className="slot-grid">
      {names.map((name, i) => {
        const occupied = i === 0 || (!disney && i === 2) || (disney && i === 1);
        return (
          <div
            className={`figure-slot${occupied ? " occupied" : ""}`}
            key={name}
          >
            <div>
              <strong>{name}</strong>
              <small className="muted">
                {occupied
                  ? i === 0
                    ? disney
                      ? "Captain Cosmo · 0x2210"
                      : "Stealth Elf · 0x0123"
                    : disney
                      ? "Nova Ranger · 0x3340"
                      : "Wash Buckler · 0x0844"
                  : "Empty"}
              </small>
            </div>
            {occupied && <div className="figure-disc" />}
            <div className="button-row">
              <button>Load</button>
              <button disabled={!occupied}>Clear</button>
            </div>
          </div>
        );
      })}
    </div>
  );
}

function UsbOverview() {
  return (
    <>
      <Notice>
        Virtual portals are attached when the HID backend initializes.
      </Notice>
      <div className="three-col usb-preview-cards">
        {[
          ["Skylanders Portal", "1430:0150", true],
          ["Disney Infinity Base", "0E6F:0129", false],
          ["LEGO Dimensions Toy Pad", "0E6F:0241", true],
        ].map(([name, id, on]) => (
          <Group title={String(name)} key={String(name)}>
            <Check checked={Boolean(on)}>Emulate this device</Check>
            <div className="mini-property">
              <span>USB ID</span>
              <b>{String(id)}</b>
            </div>
            <div className="mini-property">
              <span>State</span>
              <b className={`badge ${on ? "green" : ""}`}>
                {on ? "Attached" : "Not attached"}
              </b>
            </div>
          </Group>
        ))}
      </div>
      <Group title="Attached HID snapshot">
        <PreviewTable
          headings={["Device", "Interface", "Protocol", "RX / TX", "State"]}
          rows={[
            ["Skylanders Portal", "0", "0", "64 / 64", "Open"],
            ["LEGO Dimensions Toy Pad", "0", "0", "32 / 32", "Open"],
          ]}
        />
      </Group>
    </>
  );
}

function LegoPad() {
  return (
    <>
      <Notice>
        Drag a minifigure between pad zones or use Move to choose a destination.
      </Notice>
      <div className="diagram-pad">
        <div className="pad-zone z1">
          Left zone
          <br />2 figures
        </div>
        <div className="pad-zone z2">
          Center zone
          <br />1 figure
        </div>
        <div className="pad-zone z3">
          Right zone
          <br />0 figures
        </div>
      </div>
      <div className="button-row mt10">
        <button>Load minifigure</button>
        <button className="button-primary">Create figure</button>
        <button>Move selected</button>
        <button>Clear</button>
      </div>
    </>
  );
}

function StaticDialog({
  title,
  children,
  action,
  className,
  actionFirst = false,
  actionEmphasis = true,
}: {
  title: string;
  children: ReactNode;
  action: string;
  className?: string;
  actionFirst?: boolean;
  actionEmphasis?: boolean;
}) {
  const actionButton = (
    <button className={actionEmphasis ? "button-primary" : undefined}>
      {action}
    </button>
  );
  return (
    <dialog
      open
      className={`preview-dialog${className ? ` ${className}` : ""}`}
    >
      <header>
        <h2>{title}</h2>
        <button className="icon-button">×</button>
      </header>
      <div className="modal-body">{children}</div>
      <footer>
        {actionFirst && actionButton}
        <button>Cancel</button>
        {!actionFirst && actionButton}
      </footer>
    </dialog>
  );
}

function UsbPreview({ index }: { index: number }) {
  if (index === 63) return <UsbOverview />;
  if (index === 64 || index === 67)
    return (
      <>
        <Notice tone="success">
          The virtual Skylanders Portal is attached. Figure files are written
          immediately.
        </Notice>
        <FigureSlots />
        {index === 67 && (
          <StaticDialog title="Create figure file" action="Create">
            <FieldRow label="Figure game">
              <Select value="Skylanders" />
            </FieldRow>
            <FieldRow label="Character">
              <Select value="Stealth Elf" />
            </FieldRow>
            <FieldRow label="Variant">
              <Select value="Series 1" />
            </FieldRow>
            <FieldRow label="Save file">
              <input
                className="field wide-field"
                defaultValue="D:\Figures\stealth-elf.sky"
              />
            </FieldRow>
          </StaticDialog>
        )}
      </>
    );
  if (index === 65)
    return (
      <>
        <Notice tone="warning">
          The Disney Infinity Base is currently disabled. Enable it from
          Overview before launching a title.
        </Notice>
        <FigureSlots disney />
      </>
    );
  return (
    <>
      <LegoPad />
      {index === 68 && (
        <StaticDialog title="Move minifigure" action="Move">
          <p>
            Select the destination zone for the current LEGO Dimensions
            minifigure.
          </p>
          <div className="three-col move-zones">
            <label>
              <input type="radio" name="zone" /> Left zone
            </label>
            <label>
              <input type="radio" name="zone" defaultChecked /> Center zone
            </label>
            <label>
              <input type="radio" name="zone" /> Right zone
            </label>
          </div>
          <Check checked>Save figure file changes immediately</Check>
        </StaticDialog>
      )}
    </>
  );
}

export function ReferenceDetachedPreview({ index }: { index: number }) {
  if (index >= 30 && index <= 32) return <AccountPreview index={index} />;
  if (index === 21) return <GraphicsSettings />;
  if (index === 22) return <AudioSettings />;
  if (index === 23) return <OverlaySettings />;
  if (index === 24) return <DebugSettings />;
  if (index === 25) return <TcpGeckoSettings />;
  if (index >= 40 && index <= 43) return <TitleManagerPreview index={index} />;
  if (index >= 44 && index <= 47) return <SaveManagerPreview index={index} />;
  if (index === 48) return <DownloadsPreview />;
  if (index === 49) return <ApplicationUpdate />;
  if (index === 50) return <Catalog />;
  if (index >= 55 && index <= 59) return <DebuggerPreview index={index} />;
  if (index >= 63 && index <= 68) return <UsbPreview index={index} />;
  return null;
}

const profileNav = [
  ["General", "settings"],
  ["CPU", "tools"],
  ["Graphics", "display"],
  ["Controllers", "controller"],
] as const;

export function GameProfilePreview({ index }: { index: 75 | 76 }) {
  const controllers = index === 76;
  return (
    <div className="profile-window app-window">
      <div className="profile-shell">
        <aside className="side-nav">
          {profileNav.map(([label, icon], i) => (
            <button
              className="side-item"
              aria-current={
                (controllers ? i === 3 : i === 0) ? "page" : undefined
              }
              key={label}
            >
              <CemuIcon name={icon} />
              <span>{label}</span>
            </button>
          ))}
        </aside>
        <section className="profile-stage">
          <header className="detached-header">
            <h1>Open Air Quest</h1>
          </header>
          <div className="detached-content">
            {controllers ? <ProfileControllers /> : <ProfileGeneral />}
          </div>
          <footer className="detached-footer">
            <button>
              <CemuIcon name="help" />
              Help
            </button>
            <div className="button-row">
              <button>Revert</button>
              <button>Cancel</button>
              <button className="button-primary">Save</button>
            </div>
          </footer>
        </section>
      </div>
    </div>
  );
}

function ProfileGeneral() {
  return (
    <>
      <Notice>
        This profile overrides global settings only for Open Air Quest.
      </Notice>
      <Group title="General">
        <Check checked>Load shared libraries</Check>
        <Check checked>Start with separate GamePad view</Check>
      </Group>
      <Group title="CPU">
        <FieldRow label="CPU mode">
          <Select value="Auto" />
        </FieldRow>
        <FieldRow label="Thread quantum">
          <Select value="45,000 cycles" />
        </FieldRow>
      </Group>
      <Group title="Graphics">
        <FieldRow label="Graphics API">
          <Select value="Vulkan" />
        </FieldRow>
        <FieldRow label="Shader multiplication accuracy">
          <Select value="Use global setting" />
        </FieldRow>
      </Group>
    </>
  );
}

function ProfileControllers() {
  const rows = Array.from({ length: 8 }, (_, i) => [
    `Player ${i + 1}`,
    i === 0 ? "Wii U GamePad" : i === 1 ? "Wii U Pro Controller" : "Disabled",
    i === 0 ? "Open Air Quest" : i === 1 ? "Default Pro" : "—",
    i === 0 ? "Open Air Quest" : i === 1 ? "Default Pro" : "Disabled",
  ]);
  return (
    <>
      <Notice>Controller profiles are loaded when the title starts.</Notice>
      <div className="table-wrap">
        <table>
          <thead>
            <tr>
              {["Player", "Emulated controller", "Profile", "Selection"].map(
                (h) => (
                  <th key={h}>{h}</th>
                ),
              )}
            </tr>
          </thead>
          <tbody>
            {rows.map((row, i) => (
              <tr className={i === 0 ? "selected" : undefined} key={row[0]}>
                {row.slice(0, 3).map((v) => (
                  <td key={v}>{v}</td>
                ))}
                <td>
                  <Select value={row[3]} />
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      <div className="button-row mt10">
        <button className="button-primary">Open Input Settings</button>
        <button>Reset assignments</button>
      </div>
    </>
  );
}
