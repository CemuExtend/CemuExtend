export const isValidSavePersistentId = (value: string) =>
  /^[0-9a-fA-F]{8}$/.test(value) && Number.parseInt(value, 16) >= 0x80000001;
