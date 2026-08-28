import { mkdir } from "node:fs/promises";
import ts from "typescript";

const root = new URL("../dist/", import.meta.url);
const output = new URL(
  "../../src/webview/generated/WebAssets.h",
  import.meta.url,
);
let html = await Bun.file(new URL("index.html", root)).text();
const script = html.match(/<script[^>]+src="([^"]+)"[^>]*><\/script>/);
const style = html.match(/<link[^>]+href="([^"]+\.css)"[^>]*>/);
if (!script || !style)
  throw new Error("Vite output did not contain one script and stylesheet");
const js = await Bun.file(new URL(script[1].replace(/^\.\//, ""), root)).text();
const css = await Bun.file(new URL(style[1].replace(/^\.\//, ""), root)).text();
const embeddedJs = js.replaceAll("</script>", "<\\/script>");
html = html
  .replace(style[0], () => `<style>${css}</style>`)
  .replace(script[0], "");
if (!html.includes("</body>"))
  throw new Error("Vite output did not contain a body element");
// Vite places its module script in <head>, where it is deferred automatically.
// Once inlined as a classic script it would execute immediately, before #root
// has been parsed, so keep the self-contained bundle at the end of <body>.
html = html.replace(
  "</body>",
  () => `<script>${embeddedJs}</script>\n  </body>`,
);
const scriptHash = new Bun.CryptoHasher("sha256")
  .update(embeddedJs)
  .digest("base64");
const styleHash = new Bun.CryptoHasher("sha256").update(css).digest("base64");
const cspMeta =
  /<meta\b(?=[^>]*\bhttp-equiv\s*=\s*["']Content-Security-Policy["'])[^>]*>/gi;
const cspMatches = html.match(cspMeta);
if (cspMatches?.length !== 1)
  throw new Error("Vite output must contain exactly one CSP meta tag");
html = html.replace(
  cspMeta,
  `<meta http-equiv="Content-Security-Policy" content="default-src 'none'; script-src 'sha256-${scriptHash}'; style-src 'sha256-${styleHash}'; img-src data:; connect-src 'none'; navigate-to 'none'; base-uri 'none'; form-action 'none'" />`,
);
if (/localhost|127\.0\.0\.1:5173/.test(html))
  throw new Error("production asset contains a development server reference");
if (/unsafe-inline|connect-src[^;]*(?:https?|wss?):/.test(html))
  throw new Error("production asset contains a permissive CSP");
const javascript = ts.createSourceFile(
  "embedded.js",
  js,
  ts.ScriptTarget.Latest,
  true,
  ts.ScriptKind.JS,
);
let moduleSyntax = false;
const inspectModuleSyntax = (node: ts.Node): void => {
  if (
    ts.isImportDeclaration(node) ||
    ts.isImportEqualsDeclaration(node) ||
    ts.isExportAssignment(node) ||
    ts.isExportDeclaration(node) ||
    node.kind === ts.SyntaxKind.ImportKeyword ||
    (ts.isMetaProperty(node) &&
      node.keywordToken === ts.SyntaxKind.ImportKeyword)
  )
    moduleSyntax = true;
  ts.forEachChild(node, inspectModuleSyntax);
};
inspectModuleSyntax(javascript);
if (moduleSyntax)
  throw new Error(
    "production JavaScript is not a self-contained classic script",
  );
const finalScript = html.match(/<script>([\s\S]*)<\/script>/)?.[1];
if (!finalScript)
  throw new Error("embedded JavaScript is missing from production HTML");
const finalScriptHash = new Bun.CryptoHasher("sha256")
  .update(finalScript)
  .digest("base64");
if (
  finalScriptHash !== scriptHash ||
  !html.includes(`script-src 'sha256-${finalScriptHash}'`)
) {
  throw new Error("embedded JavaScript does not match its CSP hash");
}
const bytes = new TextEncoder().encode(html);
await mkdir(new URL("../../src/webview/generated/", import.meta.url), {
  recursive: true,
});
await Bun.write(
  output,
  `#pragma once\n#include <cstddef>\nnamespace WebAssets {\ninline constexpr unsigned char html[] = {${[...bytes].join(",")}};\ninline constexpr std::size_t htmlSize = sizeof(html);\n}\n`,
);
