import type {
  MemorySearchStatus,
  MemoryTypedValue,
  MemoryValueType,
} from "../bridge/contracts";

const integerBounds: Partial<Record<MemoryValueType, [bigint, bigint]>> = {
  int8: [-128n, 127n],
  int16: [-32768n, 32767n],
  int32: [-2147483648n, 2147483647n],
  int64: [-9223372036854775808n, 9223372036854775807n],
};

export function parseMemoryValue(
  type: MemoryValueType,
  input: string,
): MemoryTypedValue | string {
  const text = input.trim();
  if (!text) return "Enter a value to search for.";
  const bounds = integerBounds[type];
  if (bounds) {
    if (!/^-?\d+$/.test(text)) return `Enter a decimal ${type} value.`;
    try {
      const value = BigInt(text);
      if (value < bounds[0] || value > bounds[1])
        return `Value is outside the ${type} range.`;
    } catch {
      return `Enter a decimal ${type} value.`;
    }
  } else {
    const value = Number(text);
    if (!Number.isFinite(value)) return "Enter a finite floating-point value.";
  }
  return { type, text };
}

export function progressPercent(
  status: MemorySearchStatus | undefined,
): number {
  if (!status?.bytesTotal) return 0;
  return Math.min(100, (status.bytesScanned * 100) / status.bytesTotal);
}

export function pageOffset(
  page: number,
  pageSize: number,
  total: number,
): number {
  const lastPage = Math.max(0, Math.ceil(total / pageSize) - 1);
  return Math.min(Math.max(0, page), lastPage) * pageSize;
}
