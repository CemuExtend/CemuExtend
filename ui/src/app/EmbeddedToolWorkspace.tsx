import type { ImplementedToolWindowRole } from "../bridge/contracts";
import { AccountManagerWindow } from "../windows/AccountManagerWindow";
import { AboutWindow } from "../windows/AboutWindow";
import { AudioDebuggerWindow } from "../windows/AudioDebuggerWindow";
import { CemodManagerWindow } from "../windows/CemodManagerWindow";
import { ChecksumToolWindow } from "../windows/ChecksumToolWindow";
import { EmulatedUsbDevicesWindow } from "../windows/EmulatedUsbDevicesWindow";
import { GeneralSettingsWindow } from "../windows/GeneralSettingsWindow";
import { GettingStartedWindow } from "../windows/GettingStartedWindow";
import { GraphicPacksWindow } from "../windows/GraphicPacksWindow";
import { HotkeySettingsWindow } from "../windows/HotkeySettingsWindow";
import { InputSettingsWindow } from "../windows/InputSettingsWindow";
import { LoggingWindow } from "../windows/LoggingWindow";
import { MemorySearcherWindow } from "../windows/MemorySearcherWindow";
import { PpcDebuggerWindow } from "../windows/PpcDebuggerWindow";
import { PpcThreadsWindow } from "../windows/PpcThreadsWindow";
import { SaveManagerWindow } from "../windows/SaveManagerWindow";
import { TextureRelationsWindow } from "../windows/TextureRelationsWindow";
import { TitleManagerWindow } from "../windows/TitleManagerWindow";
import { UpdateManagerWindow } from "../windows/UpdateManagerWindow";
import { WorkspaceNavigationContext } from "./workspaceNavigation";
import { SCREEN_REGISTRY } from "./screenRegistry";
import { WORKSPACE_ROLES, type WorkspaceRoute } from "./workspaceRegistry";

function WorkspaceContent({
  role,
  windowId,
  context,
}: {
  role: ImplementedToolWindowRole;
  windowId: string;
  context?: { titleId?: string };
}) {
  switch (role) {
    case "about":
      return <AboutWindow />;
    case "account-manager":
      return <AccountManagerWindow />;
    case "graphic-packs":
      return (
        <GraphicPacksWindow
          windowId={windowId}
          initialTitleId={context?.titleId}
        />
      );
    case "getting-started":
      return <GettingStartedWindow />;
    case "input-settings":
      return <InputSettingsWindow />;
    case "general-settings":
      return <GeneralSettingsWindow />;
    case "hotkey-settings":
      return <HotkeySettingsWindow />;
    case "checksum-tool":
      return <ChecksumToolWindow windowId={windowId} />;
    case "title-manager":
      return <TitleManagerWindow windowId={windowId} />;
    case "cemod-manager":
      return <CemodManagerWindow windowId={windowId} />;
    case "logging":
      return <LoggingWindow />;
    case "emulated-usb-devices":
      return <EmulatedUsbDevicesWindow />;
    case "save-manager":
      return <SaveManagerWindow windowId={windowId} />;
    case "update-manager":
      return <UpdateManagerWindow windowId={windowId} />;
    case "ppc-threads":
      return <PpcThreadsWindow />;
    case "texture-relations":
      return <TextureRelationsWindow />;
    case "audio-debugger":
      return <AudioDebuggerWindow />;
    case "memory-searcher":
      return <MemorySearcherWindow />;
    case "ppc-debugger":
      return <PpcDebuggerWindow />;
    case "cemod-permissions":
      return null;
  }
}

export function EmbeddedToolWorkspace({
  route,
  role,
  windowId,
  context,
  onSelectRole,
}: {
  route: WorkspaceRoute;
  role: ImplementedToolWindowRole;
  windowId: string;
  context?: { titleId?: string };
  onSelectRole: (role: ImplementedToolWindowRole) => Promise<void>;
}) {
  const roles = WORKSPACE_ROLES[route];
  return (
    <WorkspaceNavigationContext.Provider value={onSelectRole}>
      <section className="embedded-workspace">
        {roles.length > 1 && (
          <nav className="workspace-tabs" aria-label="Workspace pages">
            {roles.map((item) => (
              <button
                key={item}
                aria-current={item === role ? "page" : undefined}
                onClick={() => void onSelectRole(item)}
              >
                {SCREEN_REGISTRY[item].title}
              </button>
            ))}
          </nav>
        )}
        <div className="embedded-workspace__content" key={role}>
          <WorkspaceContent role={role} windowId={windowId} context={context} />
        </div>
      </section>
    </WorkspaceNavigationContext.Provider>
  );
}
