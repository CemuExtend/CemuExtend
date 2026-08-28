import { mkdir, readdir, readFile, writeFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import ts from "typescript";
import { jaSupplement } from "../src/i18n/ja";
import { runtimeUiStrings } from "../translations/runtime-ui-strings";

type TranslationCache = {
  sources: string[];
  translations: Record<string, Record<string, string>>;
};
type MoEntry = { original: string; translation: string };

const scriptDirectory = dirname(fileURLToPath(import.meta.url));
const uiDirectory = resolve(scriptDirectory, "..");
const sourceDirectory = resolve(uiDirectory, "src");
const resourceDirectory = resolve(uiDirectory, "../bin/resources");
const cachePath = resolve(uiDirectory, "translations/react-ui.json");
const translatedAttributes = new Set(["aria-label", "placeholder", "title"]);
const excludedLiterals = new Set([
  "CE",
  "CemuExtend",
  "SDL",
  "Wii U",
  "WebView",
  "PPC",
  "CPU",
  "GPU",
  "NFC",
  "USB",
  "SHA-256",
]);
const googleLanguage: Record<string, string> = { nb: "no", zh: "zh-CN" };

function normalizeText(value: string): string {
  return value
    .replaceAll("&apos;", "'")
    .replaceAll("&quot;", '"')
    .replaceAll("&amp;", "&")
    .replace(/\s+/gu, " ")
    .trim();
}

function shouldTranslate(value: string): boolean {
  if (
    value.length < 2 ||
    excludedLiterals.has(value) ||
    !/[A-Za-z]/u.test(value) ||
    /^(?:https?:|\/|\.|[a-z]+(?:[A-Z][a-z]+)+$)/u.test(value) ||
    /^(?:[a-z0-9]+[-_.:]){2,}[a-z0-9]+$/iu.test(value)
  )
    return false;
  return true;
}

async function sourceFiles(directory: string): Promise<string[]> {
  const files: string[] = [];
  for (const entry of await readdir(directory, { withFileTypes: true })) {
    const path = resolve(directory, entry.name);
    if (entry.isDirectory()) files.push(...(await sourceFiles(path)));
    else if (entry.name.endsWith(".tsx")) files.push(path);
  }
  return files;
}

function addLiteral(target: Set<string>, value: string): void {
  const normalized = normalizeText(value);
  if (shouldTranslate(normalized)) target.add(normalized);
}

function collectFromSource(
  path: string,
  text: string,
  target: Set<string>,
): void {
  const source = ts.createSourceFile(
    path,
    text,
    ts.ScriptTarget.Latest,
    true,
    path.endsWith(".tsx") ? ts.ScriptKind.TSX : ts.ScriptKind.TS,
  );
  const collectStrings =
    path.endsWith("i18n/en.ts") || path.endsWith("screenRegistry.ts");
  const visit = (node: ts.Node, insideJsxExpression = false): void => {
    if (ts.isJsxText(node)) addLiteral(target, node.getText(source));
    else if (
      ts.isJsxAttribute(node) &&
      translatedAttributes.has(node.name.getText(source)) &&
      node.initializer &&
      ts.isStringLiteral(node.initializer)
    )
      addLiteral(target, node.initializer.text);
    else if (
      ts.isStringLiteralLike(node) &&
      (collectStrings || insideJsxExpression)
    )
      addLiteral(target, node.text);
    const nextInside = insideJsxExpression || ts.isJsxExpression(node);
    ts.forEachChild(node, (child) => visit(child, nextInside));
  };
  visit(source);
}

async function collectUiSources(): Promise<string[]> {
  const values = new Set<string>();
  runtimeUiStrings.forEach((value) => values.add(value));
  for (const path of await sourceFiles(sourceDirectory))
    collectFromSource(path, await readFile(path, "utf8"), values);
  for (const relative of ["src/i18n/en.ts", "src/app/screenRegistry.ts"])
    collectFromSource(
      relative,
      await readFile(resolve(uiDirectory, relative), "utf8"),
      values,
    );
  return [...values].sort((left, right) => left.localeCompare(right, "en"));
}

function parseMo(bytes: Uint8Array): MoEntry[] {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const magic = view.getUint32(0, true);
  const littleEndian = magic === 0x950412de;
  if (!littleEndian && magic !== 0xde120495)
    throw new Error("invalid GNU MO catalog");
  const count = view.getUint32(8, littleEndian);
  const originalTable = view.getUint32(12, littleEndian);
  const translatedTable = view.getUint32(16, littleEndian);
  const decoder = new TextDecoder();
  const read = (table: number, index: number) => {
    const length = view.getUint32(table + index * 8, littleEndian);
    const offset = view.getUint32(table + index * 8 + 4, littleEndian);
    return decoder.decode(bytes.subarray(offset, offset + length));
  };
  return Array.from({ length: count }, (_, index) => ({
    original: read(originalTable, index),
    translation: read(translatedTable, index),
  }));
}

function writeMo(entries: MoEntry[]): Uint8Array {
  const encoder = new TextEncoder();
  const sorted = [...entries].sort((left, right) =>
    Buffer.compare(Buffer.from(left.original), Buffer.from(right.original)),
  );
  const originals = sorted.map(({ original }) => encoder.encode(original));
  const translations = sorted.map(({ translation }) =>
    encoder.encode(translation),
  );
  const count = sorted.length;
  const headerSize = 28;
  const originalTable = headerSize;
  const translatedTable = originalTable + count * 8;
  let stringOffset = translatedTable + count * 8;
  const originalOffsets = originals.map((value) => {
    const offset = stringOffset;
    stringOffset += value.length + 1;
    return offset;
  });
  const translatedOffsets = translations.map((value) => {
    const offset = stringOffset;
    stringOffset += value.length + 1;
    return offset;
  });
  const output = new Uint8Array(stringOffset);
  const view = new DataView(output.buffer);
  view.setUint32(0, 0x950412de, true);
  view.setUint32(4, 0, true);
  view.setUint32(8, count, true);
  view.setUint32(12, originalTable, true);
  view.setUint32(16, translatedTable, true);
  view.setUint32(20, 0, true);
  view.setUint32(24, 0, true);
  originals.forEach((value, index) => {
    view.setUint32(originalTable + index * 8, value.length, true);
    view.setUint32(originalTable + index * 8 + 4, originalOffsets[index], true);
    output.set(value, originalOffsets[index]);
  });
  translations.forEach((value, index) => {
    view.setUint32(translatedTable + index * 8, value.length, true);
    view.setUint32(
      translatedTable + index * 8 + 4,
      translatedOffsets[index],
      true,
    );
    output.set(value, translatedOffsets[index]);
  });
  return output;
}

function decodeHtml(value: string): string {
  return value
    .replaceAll("&quot;", '"')
    .replaceAll("&#39;", "'")
    .replaceAll("&lt;", "<")
    .replaceAll("&gt;", ">")
    .replaceAll("&amp;", "&")
    .replace(/&#(\d+);/gu, (_, code: string) =>
      String.fromCodePoint(Number(code)),
    );
}

async function translateBatch(
  language: string,
  sources: string[],
): Promise<string[]> {
  const placeholders = sources.map(
    (source) => source.match(/\{[^}]+\}/gu) ?? [],
  );
  const querySources = sources.map((source, sourceIndex) => {
    let protectedSource = source;
    placeholders[sourceIndex].forEach((placeholder, placeholderIndex) => {
      protectedSource = protectedSource.replaceAll(
        placeholder,
        `__CEMUVAR${placeholderIndex}__`,
      );
    });
    return protectedSource;
  });
  const restorePlaceholders = (value: string, sourceIndex: number) => {
    let restored = value;
    placeholders[sourceIndex].forEach((placeholder, placeholderIndex) => {
      restored = restored.replaceAll(
        `__CEMUVAR${placeholderIndex}__`,
        placeholder,
      );
    });
    return restored;
  };
  const query =
    querySources.length === 1
      ? querySources[0]
      : querySources
          .map(
            (source, index) =>
              `__CEMU_${String(index).padStart(3, "0")}__ ${source}`,
          )
          .join("\n");
  const url = new URL("https://translate.google.com/m");
  url.search = new URLSearchParams({
    sl: "en",
    tl: googleLanguage[language] ?? language,
    q: query,
  }).toString();
  let response: Response | undefined;
  for (let attempt = 0; attempt < 4; attempt += 1) {
    response = await fetch(url, {
      headers: { "user-agent": "Mozilla/5.0" },
      signal: AbortSignal.timeout(15_000),
    }).catch(() => undefined);
    if (response?.ok) break;
    await Bun.sleep(500 * 2 ** attempt);
  }
  const successfulResponse = response;
  if (!successfulResponse?.ok)
    throw new Error(
      `translation request failed: ${successfulResponse?.status ?? "network"}`,
    );
  const html = await successfulResponse.text();
  const translated = decodeHtml(
    html.match(/<div class="result-container">([\s\S]*?)<\/div>/u)?.[1] ?? "",
  );
  if (sources.length === 1) {
    if (!translated.trim()) throw new Error("translation result was empty");
    return [restorePlaceholders(translated.trim(), 0)];
  }
  const results: string[] = [];
  for (let index = 0; index < sources.length; index += 1) {
    const marker = `__CEMU_${String(index).padStart(3, "0")}__ `;
    const start = translated.indexOf(marker);
    const nextMarker = `\n__CEMU_${String(index + 1).padStart(3, "0")}__ `;
    const end =
      index + 1 === sources.length
        ? translated.length
        : translated.indexOf(nextMarker);
    if (start < 0 || end < 0) {
      const middle = Math.ceil(sources.length / 2);
      return [
        ...(await translateBatch(language, sources.slice(0, middle))),
        ...(await translateBatch(language, sources.slice(middle))),
      ];
    }
    results.push(translated.slice(start + marker.length, end).trim());
  }
  return results.map(restorePlaceholders);
}

async function translateMissing(
  language: string,
  sources: string[],
  catalog: Record<string, string>,
): Promise<void> {
  const missing = sources.filter((source) => !catalog[source]);
  const batchSize = 24;
  const batches = Array.from(
    { length: Math.ceil(missing.length / batchSize) },
    (_, index) => missing.slice(index * batchSize, (index + 1) * batchSize),
  );
  let nextBatch = 0;
  const worker = async () => {
    for (;;) {
      const index = nextBatch;
      nextBatch += 1;
      if (index >= batches.length) return;
      const batch = batches[index];
      const translated = await translateBatch(language, batch);
      batch.forEach((source, sourceIndex) => {
        catalog[source] = translated[sourceIndex];
      });
      console.log(`${language}: batch ${index + 1}/${batches.length}`);
    }
  };
  await Promise.all(
    Array.from({ length: Math.min(6, batches.length) }, worker),
  );
}

const sources = await collectUiSources();
await mkdir(dirname(cachePath), { recursive: true });
const cache: TranslationCache = (await Bun.file(cachePath).exists())
  ? ((await Bun.file(cachePath).json()) as TranslationCache)
  : { sources: [], translations: {} };
cache.sources = sources;

for (const languageEntry of await readdir(resourceDirectory, {
  withFileTypes: true,
})) {
  if (!languageEntry.isDirectory() || languageEntry.name === "en") continue;
  const directory = resolve(resourceDirectory, languageEntry.name);
  const moName = (await readdir(directory)).find((name) =>
    name.endsWith(".mo"),
  );
  if (!moName) continue;
  const moPath = resolve(directory, moName);
  const entries = parseMo(await readFile(moPath));
  const existing = new Map(entries.map((entry) => [entry.original, entry]));
  const translations = (cache.translations[languageEntry.name] ??= {});
  for (const source of sources) {
    const current = existing.get(source)?.translation;
    if (current) translations[source] = current.split("\0", 1)[0];
  }
  if (languageEntry.name === "ja") Object.assign(translations, jaSupplement);
  await translateMissing(languageEntry.name, sources, translations);
  for (const source of sources) {
    const translation = translations[source];
    if (!translation) continue;
    const current = existing.get(source);
    if (current) current.translation = translation;
    else entries.push({ original: source, translation });
  }
  await writeFile(moPath, writeMo(entries));
  await writeFile(cachePath, `${JSON.stringify(cache, null, 2)}\n`);
  console.log(`${languageEntry.name}: ${sources.length} React UI messages`);
}

await writeFile(cachePath, `${JSON.stringify(cache, null, 2)}\n`);
