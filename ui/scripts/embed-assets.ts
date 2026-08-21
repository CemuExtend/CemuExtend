import { mkdir } from "node:fs/promises";

const root = new URL("../dist/", import.meta.url);
const output = new URL("../../src/webview/generated/WebAssets.h", import.meta.url);
let html = await Bun.file(new URL("index.html", root)).text();
const script = html.match(/<script[^>]+src="([^"]+)"[^>]*><\/script>/);
const style = html.match(/<link[^>]+href="([^"]+\.css)"[^>]*>/);
if (!script || !style) throw new Error("Vite output did not contain one script and stylesheet");
const js = await Bun.file(new URL(script[1].replace(/^\.\//, ""), root)).text();
const css = await Bun.file(new URL(style[1].replace(/^\.\//, ""), root)).text();
const embeddedJs = js.replaceAll("</script>", "<\\/script>");
html = html
  .replace(style[0], () => `<style>${css}</style>`)
  .replace(script[0], () => `<script>${embeddedJs}</script>`);
const scriptHash = new Bun.CryptoHasher("sha256").update(embeddedJs).digest("base64");
const styleHash = new Bun.CryptoHasher("sha256").update(css).digest("base64");
const sourceHtml = html;
html = html.replace(/<meta http-equiv="Content-Security-Policy"[^>]*>/, `<meta http-equiv="Content-Security-Policy" content="default-src 'none'; script-src 'sha256-${scriptHash}'; style-src 'sha256-${styleHash}'; img-src data:; connect-src 'none'; navigate-to 'none'; base-uri 'none'; form-action 'none'" />`);
if (html === sourceHtml || !html.includes("Content-Security-Policy")) throw new Error("production CSP meta tag was not replaced exactly once");
if (/localhost|127\.0\.0\.1:5173/.test(html)) throw new Error("production asset contains a development server reference");
if (/unsafe-inline|connect-src[^;]*(?:https?|wss?):/.test(html)) throw new Error("production asset contains a permissive CSP");
if (/\bimport\s*(?:\(|[{'*"])|import\.meta/.test(js)) throw new Error("production JavaScript is not a self-contained classic script");
const finalScript = html.match(/<script>([\s\S]*)<\/script>/)?.[1];
if (!finalScript) throw new Error("embedded JavaScript is missing from production HTML");
const finalScriptHash = new Bun.CryptoHasher("sha256").update(finalScript).digest("base64");
if (finalScriptHash !== scriptHash || !html.includes(`script-src 'sha256-${finalScriptHash}'`)) {
  throw new Error("embedded JavaScript does not match its CSP hash");
}
const bytes = new TextEncoder().encode(html);
await mkdir(new URL("../../src/webview/generated/", import.meta.url), { recursive: true });
await Bun.write(output, `#pragma once\n#include <cstddef>\nnamespace WebAssets {\ninline constexpr unsigned char html[] = {${[...bytes].join(",")}};\ninline constexpr std::size_t htmlSize = sizeof(html);\n}\n`);
