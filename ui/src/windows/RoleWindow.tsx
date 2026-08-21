import type { ImplementedToolWindowRole } from "../bridge/contracts";
import { AboutWindow } from "./AboutWindow";
import { AccountManagerWindow } from "./AccountManagerWindow";
import { GraphicPacksWindow } from "./GraphicPacksWindow";

export function RoleWindow({ role, windowId, context }: { role: ImplementedToolWindowRole; windowId: number; context?: { titleId?: string } }) {
  switch (role) {
    case "about": return <AboutWindow />;
    case "account-manager": return <AccountManagerWindow />;
    case "graphic-packs": return <GraphicPacksWindow windowId={windowId} initialTitleId={context?.titleId} />;
  }
}
