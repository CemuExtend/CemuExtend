import { mkdir } from "node:fs/promises";

await mkdir(new URL("../src/generated/", import.meta.url), { recursive: true });
await Bun.write(
  new URL("../src/generated/translations.json", import.meta.url),
  JSON.stringify({ en: {} }),
);
