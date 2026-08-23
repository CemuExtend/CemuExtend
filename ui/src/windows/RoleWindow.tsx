import type { ImplementedToolWindowRole } from "../bridge/contracts";
import { DetachedToolShell } from "../app/DetachedToolShell";
import { SCREEN_REGISTRY } from "../app/screenRegistry";
import { AboutWindow } from "./AboutWindow";
import { AccountManagerWindow } from "./AccountManagerWindow";
import { GraphicPacksWindow } from "./GraphicPacksWindow";
import { GettingStartedWindow } from "./GettingStartedWindow";
import { InputSettingsWindow } from "./InputSettingsWindow";
import { GeneralSettingsWindow } from "./GeneralSettingsWindow";
import { HotkeySettingsWindow } from "./HotkeySettingsWindow";
import { ChecksumToolWindow } from "./ChecksumToolWindow";
import { TitleManagerWindow } from "./TitleManagerWindow";
import { CemodManagerWindow } from "./CemodManagerWindow";
import { CemodPermissionsWindow } from "./CemodPermissionsWindow";
import { LoggingWindow } from "./LoggingWindow";
import { EmulatedUsbDevicesWindow } from "./EmulatedUsbDevicesWindow";
import { SaveManagerWindow } from "./SaveManagerWindow";
import { UpdateManagerWindow } from "./UpdateManagerWindow";
import { PpcThreadsWindow } from "./PpcThreadsWindow";
import { TextureRelationsWindow } from "./TextureRelationsWindow";
import { AudioDebuggerWindow } from "./AudioDebuggerWindow";
import { MemorySearcherWindow } from "./MemorySearcherWindow";
import { PpcDebuggerWindow } from "./PpcDebuggerWindow";

export function RoleWindow({
  role,
  windowId,
  context,
}: {
  role: ImplementedToolWindowRole;
  windowId: string;
  context?: { titleId?: string; packageKey?: string; generation?: string };
}) {
  const content = (() => {
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
      case "cemod-permissions":
        return <CemodPermissionsWindow windowId={windowId} context={context} />;
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
    }
  })();
  return (
    <DetachedToolShell definition={SCREEN_REGISTRY[role]}>
      {content}
    </DetachedToolShell>
  );
}
