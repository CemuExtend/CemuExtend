import type { ImplementedToolWindowRole } from "../../bridge/contracts";

export type ToolContext = { titleId?: string };
export type OpenToolHandler = (
  role: ImplementedToolWindowRole,
  context?: ToolContext,
) => void;
