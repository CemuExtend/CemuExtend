import builder from "./covers/builder.png?inline";
import cosmo from "./covers/cosmo.png?inline";
import garden from "./covers/garden.png?inline";
import homebrew from "./covers/homebrew.png?inline";
import kart from "./covers/kart.png?inline";
import media from "./covers/media.png?inline";
import nus from "./covers/nus.png?inline";
import openAir from "./covers/open_air.png?inline";
import splatter from "./covers/splatter.png?inline";
import starship from "./covers/starship.png?inline";
import system from "./covers/system.png?inline";
import wind from "./covers/wind.png?inline";

export const titleCovers = {
  builder,
  cosmo,
  garden,
  homebrew,
  kart,
  media,
  nus,
  open_air: openAir,
  splatter,
  starship,
  system,
  wind,
} as const;

export type TitleCoverName = keyof typeof titleCovers;

export function titleCover(name: string): string {
  return titleCovers[name as TitleCoverName] ?? openAir;
}
