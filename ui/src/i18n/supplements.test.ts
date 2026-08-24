import { expect, test } from "bun:test";
import bundle from "../generated/translations.json";
import {
  contextualUiSupplementSources,
  contextualUiSupplements,
  developerUiSupplementSources,
  developerUiSupplements,
  uiSupplementSources,
  uiSupplements,
} from "./supplements";

const languageCodes = Object.keys(bundle.messages)
  .filter((language) => language !== "en")
  .sort();

test("UI supplements cover every shipped non-English language", () => {
  expect(Object.keys(uiSupplements).sort()).toEqual(languageCodes);
  expect(Object.keys(contextualUiSupplements).sort()).toEqual(languageCodes);
  expect(Object.keys(developerUiSupplements).sort()).toEqual(languageCodes);
  for (const language of languageCodes) {
    const supplement = uiSupplements[language as keyof typeof uiSupplements];
    const contextual =
      contextualUiSupplements[language as keyof typeof contextualUiSupplements];
    const developer =
      developerUiSupplements[language as keyof typeof developerUiSupplements];
    expect(Object.keys(supplement).sort()).toEqual(
      [...uiSupplementSources].sort(),
    );
    for (const source of uiSupplementSources) {
      const translated = supplement[source];
      expect(translated.trim().length).toBeGreaterThan(0);
      for (const placeholder of source.matchAll(/\{[^}]+\}/gu))
        expect(translated).toContain(placeholder[0]);
    }
    expect(Object.keys(contextual).sort()).toEqual(
      [...contextualUiSupplementSources].sort(),
    );
    for (const source of contextualUiSupplementSources) {
      const translated = contextual[source];
      expect(translated.trim().length).toBeGreaterThan(0);
      for (const placeholder of source.matchAll(/\{[^}]+\}/gu))
        expect(translated).toContain(placeholder[0]);
    }
    expect(Object.keys(developer).sort()).toEqual(
      [...developerUiSupplementSources].sort(),
    );
    for (const source of developerUiSupplementSources) {
      const translated = developer[source];
      expect(translated.trim().length).toBeGreaterThan(0);
      for (const placeholder of source.matchAll(/\{[^}]+\}/gu))
        expect(translated).toContain(placeholder[0]);
    }
  }
});

test("existing catalogs cover the title filters and mapping actions", () => {
  const required = [
    "Filter titles, IDs, or paths",
    "All content",
    "Capture",
    "Clear",
    "Mappings",
  ];
  for (const language of languageCodes) {
    const catalog = bundle.messages[language as keyof typeof bundle.messages];
    const folded = new Set(
      Object.keys(catalog).map((key) => key.toLowerCase()),
    );
    for (const source of required)
      expect(source in catalog || folded.has(source.toLowerCase())).toBeTrue();
  }
});
