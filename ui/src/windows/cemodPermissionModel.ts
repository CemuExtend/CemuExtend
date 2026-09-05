import type { CemodPackage } from "../bridge/contracts";

type PermissionModel = Pick<
  CemodPackage,
  "permissions" | "requestedPermissions"
>;

export function isCemodPermissionModelValid(model: PermissionModel): boolean {
  try {
    const requestedPermissions = BigInt(model.requestedPermissions);
    if (requestedPermissions < 0n) return false;

    let representedPermissions = 0n;
    let requestedRows = 0n;
    for (const permission of model.permissions) {
      const bit = BigInt(permission.bit);
      if (
        bit <= 0n ||
        (bit & (bit - 1n)) !== 0n ||
        (representedPermissions & bit) !== 0n
      )
        return false;

      representedPermissions |= bit;
      if (permission.requested) requestedRows |= bit;
    }

    return (
      (requestedPermissions & ~representedPermissions) === 0n &&
      requestedRows === requestedPermissions
    );
  } catch {
    return false;
  }
}
