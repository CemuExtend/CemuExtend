import type { GuestAddress } from "../bridge/contracts";

export function parseGuestAddress(value: string): GuestAddress | undefined {
  const normalized = value.trim().replace(/^0x/i, "");
  return /^[0-9a-fA-F]{1,8}$/.test(normalized) ? normalized.padStart(8, "0").toUpperCase() : undefined;
}

export function centerAddress(address: GuestAddress, deltaInstructions: number): GuestAddress {
  const value = Number.parseInt(address, 16);
  return Math.max(0, Math.min(0xffff_fffc, value + deltaInstructions * 4)).toString(16).padStart(8, "0").toUpperCase();
}
