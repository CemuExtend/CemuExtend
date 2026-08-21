import type { ImplementedToolWindowRole } from "../bridge/contracts";
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

export function RoleWindow({ role, windowId, context }: { role: ImplementedToolWindowRole; windowId: string; context?: { titleId?: string; packageKey?: string; generation?: string } }) {
  switch (role) {
    case "about": return <AboutWindow />;
    case "account-manager": return <AccountManagerWindow />;
    case "graphic-packs": return <GraphicPacksWindow windowId={windowId} initialTitleId={context?.titleId} />;
    case "getting-started": return <GettingStartedWindow />;
    case "input-settings": return <InputSettingsWindow />;
    case "general-settings": return <GeneralSettingsWindow />;
    case "hotkey-settings": return <HotkeySettingsWindow />;
    case "checksum-tool": return <ChecksumToolWindow windowId={windowId} />;
    case "title-manager": return <TitleManagerWindow windowId={windowId} />;
    case "cemod-manager": return <CemodManagerWindow windowId={windowId} />;
    case "cemod-permissions": return <CemodPermissionsWindow windowId={windowId} context={context} />;
    case "logging": return <LoggingWindow />;
  }
}
