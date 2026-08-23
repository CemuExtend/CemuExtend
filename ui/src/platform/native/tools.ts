import type { ImplementedToolWindowRole } from "../../bridge/contracts";
import { openWindow } from "../../bridge/windows";

export type ToolContext = { titleId?: string };
export type OpenToolHandler = (
  role: ImplementedToolWindowRole,
  context?: ToolContext,
) => void;

export function openTool(
  role: ImplementedToolWindowRole,
  context?: ToolContext,
): Promise<string> {
  if (role === "graphic-packs") return openWindow(role, context);
  return openWindow(role);
}
