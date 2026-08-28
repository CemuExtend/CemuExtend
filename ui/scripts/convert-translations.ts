import { mkdir, readdir, readFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

type Catalog = Record<string, string>;

const languageNames: Record<string, string> = {
  en: "English",
  ar: "العربية",
  ca: "Català",
  de: "Deutsch",
  es: "Español",
  fr: "Français",
  he: "עברית",
  hu: "Magyar",
  it: "Italiano",
  ja: "日本語",
  ko: "한국어",
  nb: "Norsk bokmål",
  nl: "Nederlands",
  pl: "Polski",
  pt: "Português",
  ru: "Русский",
  sv: "Svenska",
  tr: "Türkçe",
  uk: "Українська",
  zh: "中文",
};

function removeMnemonic(value: string): string {
  const escaped = value.replaceAll("&&", "\0");
  return escaped
    .replace(/\(&.\)/gu, "")
    .replaceAll("&", "")
    .replaceAll("\0", "&")
    .trim();
}

function parseMo(bytes: Uint8Array): Catalog {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const magic = view.getUint32(0, true);
  const littleEndian = magic === 0x950412de;
  if (!littleEndian && magic !== 0xde120495)
    throw new Error("invalid GNU MO magic");
  const count = view.getUint32(8, littleEndian);
  const originalTable = view.getUint32(12, littleEndian);
  const translatedTable = view.getUint32(16, littleEndian);
  const decoder = new TextDecoder("utf-8");
  const readEntry = (table: number, index: number) => {
    const entry = table + index * 8;
    const length = view.getUint32(entry, littleEndian);
    const offset = view.getUint32(entry + 4, littleEndian);
    if (offset + length > bytes.byteLength)
      throw new Error("GNU MO entry is outside the catalog");
    return decoder.decode(bytes.subarray(offset, offset + length));
  };
  const catalog: Catalog = {};
  for (let index = 0; index < count; index += 1) {
    const source = readEntry(originalTable, index).split("\0", 1)[0];
    const translated = readEntry(translatedTable, index).split("\0", 1)[0];
    if (!source || !translated) continue;
    catalog[source] = removeMnemonic(translated);
    const plainSource = removeMnemonic(source);
    if (plainSource && !(plainSource in catalog))
      catalog[plainSource] = removeMnemonic(translated);
  }
  return catalog;
}

const scriptDirectory = dirname(fileURLToPath(import.meta.url));
const resourcesDirectory = resolve(scriptDirectory, "../../bin/resources");
const generatedDirectory = resolve(scriptDirectory, "../src/generated");
const entries = await readdir(resourcesDirectory, { withFileTypes: true });
const messages: Record<string, Catalog> = { en: {} };

for (const entry of entries) {
  if (!entry.isDirectory() || !(entry.name in languageNames)) continue;
  const files = await readdir(resolve(resourcesDirectory, entry.name));
  const catalogFile = files.find((file) => file.endsWith(".mo"));
  if (!catalogFile) continue;
  messages[entry.name] = parseMo(
    await readFile(resolve(resourcesDirectory, entry.name, catalogFile)),
  );
}

const languages = Object.keys(messages)
  .sort((left, right) => left.localeCompare(right))
  .map((code) => ({ code, name: languageNames[code] }));

await mkdir(generatedDirectory, { recursive: true });
await Bun.write(
  resolve(generatedDirectory, "translations.json"),
  `${JSON.stringify({ languages, messages })}\n`,
);
