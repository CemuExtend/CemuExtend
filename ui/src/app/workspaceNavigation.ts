import { createContext, useContext } from "react";
import type { ImplementedToolWindowRole } from "../bridge/contracts";

export type WorkspaceNavigate = (
  role: ImplementedToolWindowRole,
) => Promise<void>;

export const WorkspaceNavigationContext = createContext<
  WorkspaceNavigate | undefined
>(undefined);

export function useWorkspaceNavigation() {
  const navigate = useContext(WorkspaceNavigationContext);
  if (!navigate)
    throw new Error("Workspace navigation requires the main application shell");
  return navigate;
}
