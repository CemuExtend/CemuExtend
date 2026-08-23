import bundle from "../generated/translations.json";

export type UiLanguage = { code: string; name: string };

const systemLanguage: UiLanguage = {
  code: "system",
  name: "Default",
};
const languages = bundle.languages as UiLanguage[];
const messages = bundle.messages as Record<string, Record<string, string>>;
const foldedMessages = Object.fromEntries(
  Object.entries(messages).map(([language, catalog]) => [
    language,
    Object.fromEntries(
      Object.entries(catalog).map(([source, translated]) => [
        source.toLocaleLowerCase("en"),
        translated,
      ]),
    ),
  ]),
) as Record<string, Record<string, string>>;
const supported = new Set(languages.map(({ code }) => code));
const textState = new WeakMap<Text, { source: string; translated: string }>();
const attributeState = new WeakMap<Element, Map<string, { source: string; translated: string }>>();
const translatedAttributes = ["aria-label", "placeholder", "title"] as const;
const rightToLeftLanguages = new Set(["ar", "he"]);
let selectedLanguage = "system";
let activeLanguage = "en";
let observer: MutationObserver | undefined;

export const uiLanguages: readonly UiLanguage[] = [systemLanguage, ...languages];

function normalizeLanguage(value: string): string {
  const candidate = value.trim().toLowerCase().replace("_", "-");
  if (candidate === "system" || supported.has(candidate)) return candidate;
  const base = candidate.split("-", 1)[0];
  return supported.has(base) ? base : "en";
}

function resolveSystemLanguage(): string {
  for (const candidate of navigator.languages) {
    const resolved = normalizeLanguage(candidate);
    if (resolved !== "en" || candidate.toLowerCase().startsWith("en"))
      return resolved;
  }
  return "en";
}

export function translate(source: string): string {
  if (activeLanguage === "en") return source;
  return (
    messages[activeLanguage]?.[source] ??
    foldedMessages[activeLanguage]?.[source.toLocaleLowerCase("en")] ??
    source
  );
}

export function translateFormat(
  source: string,
  values: Record<string, string | number>,
): string {
  let translated = translate(source);
  for (const [name, value] of Object.entries(values))
    translated = translated.replaceAll(`{${name}}`, String(value));
  return translated;
}

function translateText(node: Text): void {
  const parent = node.parentElement;
  if (!parent || parent.closest("script, style, [data-i18n-ignore], [contenteditable='true']"))
    return;
  const previous = textState.get(node);
  const current = node.data;
  const source = previous && current === previous.translated ? previous.source : current;
  const match = /^(\s*)(.*?)(\s*)$/su.exec(source);
  if (!match || !match[2]) return;
  const translated = `${match[1]}${translate(match[2])}${match[3]}`;
  textState.set(node, { source, translated });
  if (translated !== current) node.data = translated;
}

function translateAttributes(element: Element): void {
  let state = attributeState.get(element);
  if (!state) {
    state = new Map();
    attributeState.set(element, state);
  }
  for (const name of translatedAttributes) {
    const current = element.getAttribute(name);
    if (current === null) continue;
    const previous = state.get(name);
    const source = previous && current === previous.translated ? previous.source : current;
    const translated = translate(source);
    state.set(name, { source, translated });
    if (translated !== current) element.setAttribute(name, translated);
  }
}

function localize(root: Node): void {
  if (root.nodeType === Node.TEXT_NODE) {
    translateText(root as Text);
    return;
  }
  if (root.nodeType !== Node.ELEMENT_NODE && root.nodeType !== Node.DOCUMENT_FRAGMENT_NODE)
    return;
  if (root instanceof Element) translateAttributes(root);
  const walker = document.createTreeWalker(
    root,
    NodeFilter.SHOW_ELEMENT | NodeFilter.SHOW_TEXT,
  );
  for (let node = walker.nextNode(); node; node = walker.nextNode()) {
    if (node.nodeType === Node.TEXT_NODE) translateText(node as Text);
    else translateAttributes(node as Element);
  }
}

export function setUiLanguage(language: string): void {
  selectedLanguage = normalizeLanguage(language);
  activeLanguage =
    selectedLanguage === "system" ? resolveSystemLanguage() : selectedLanguage;
  document.documentElement.lang = activeLanguage;
  document.documentElement.dir = rightToLeftLanguages.has(activeLanguage)
    ? "rtl"
    : "ltr";
  localize(document.body);
}

export function getUiLanguage(): string {
  return selectedLanguage;
}

export function installLocalization(language: string): void {
  setUiLanguage(language);
  if (observer) return;
  observer = new MutationObserver((records) => {
    for (const record of records) {
      if (record.type === "characterData") localize(record.target);
      else if (record.type === "attributes") translateAttributes(record.target as Element);
      else record.addedNodes.forEach(localize);
    }
  });
  observer.observe(document.body, {
    subtree: true,
    childList: true,
    characterData: true,
    attributes: true,
    attributeFilter: [...translatedAttributes],
  });
}
