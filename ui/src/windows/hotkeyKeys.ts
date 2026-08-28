const codeUsage = new Map<string, number>();
for (let index = 0; index < 26; index += 1)
  codeUsage.set(`Key${String.fromCharCode(65 + index)}`, 0x04 + index);
for (let index = 1; index <= 9; index += 1)
  codeUsage.set(`Digit${index}`, 0x1d + index);
codeUsage.set("Digit0", 0x27);
[
  ["Enter", 0x28],
  ["Escape", 0x29],
  ["Backspace", 0x2a],
  ["Tab", 0x2b],
  ["Space", 0x2c],
  ["Minus", 0x2d],
  ["Equal", 0x2e],
  ["BracketLeft", 0x2f],
  ["BracketRight", 0x30],
  ["Backslash", 0x31],
  ["Semicolon", 0x33],
  ["Quote", 0x34],
  ["Backquote", 0x35],
  ["Comma", 0x36],
  ["Period", 0x37],
  ["Slash", 0x38],
  ["CapsLock", 0x39],
  ["PrintScreen", 0x46],
  ["ScrollLock", 0x47],
  ["Pause", 0x48],
  ["Insert", 0x49],
  ["Home", 0x4a],
  ["PageUp", 0x4b],
  ["Delete", 0x4c],
  ["End", 0x4d],
  ["PageDown", 0x4e],
  ["ArrowRight", 0x4f],
  ["ArrowLeft", 0x50],
  ["ArrowDown", 0x51],
  ["ArrowUp", 0x52],
  ["NumLock", 0x53],
  ["NumpadDivide", 0x54],
  ["NumpadMultiply", 0x55],
  ["NumpadSubtract", 0x56],
  ["NumpadAdd", 0x57],
  ["NumpadEnter", 0x58],
  ["Numpad1", 0x59],
  ["Numpad2", 0x5a],
  ["Numpad3", 0x5b],
  ["Numpad4", 0x5c],
  ["Numpad5", 0x5d],
  ["Numpad6", 0x5e],
  ["Numpad7", 0x5f],
  ["Numpad8", 0x60],
  ["Numpad9", 0x61],
  ["Numpad0", 0x62],
  ["NumpadDecimal", 0x63],
  ["ContextMenu", 0x65],
].forEach(([code, usage]) => codeUsage.set(code as string, usage as number));
for (let index = 1; index <= 12; index += 1)
  codeUsage.set(`F${index}`, 0x39 + index);
for (let index = 13; index <= 24; index += 1)
  codeUsage.set(`F${index}`, 0x68 + index - 13);
const usageCode = new Map(
  Array.from(codeUsage, ([code, usage]) => [usage, code]),
);

export const hotkeyUsageForCode = (code: string) => codeUsage.get(code);

export function hotkeyKeyboardLabel(binding: {
  keyboardUsage: number;
  keyboardModifiers: number;
}) {
  if (!binding.keyboardUsage) return "Unassigned";
  const parts: string[] = [];
  if (binding.keyboardModifiers & 1) parts.push("Ctrl");
  if (binding.keyboardModifiers & 2) parts.push("Shift");
  if (binding.keyboardModifiers & 4) parts.push("Alt");
  if (binding.keyboardModifiers & 8) parts.push("Meta");
  parts.push(
    usageCode
      .get(binding.keyboardUsage)
      ?.replace(/^Key/, "")
      .replace(/^Digit/, "") ?? `HID 0x${binding.keyboardUsage.toString(16)}`,
  );
  return parts.join(" + ");
}
