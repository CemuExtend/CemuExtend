# CemuExtend

**English** | [日本語](README.ja.md)

<p align="center">
  <img src="docs/assets/cemuextend-logo.png" alt="CemuExtend logo" width="1000">
</p>

**Play Wii U games with more room to mod, create, and experiment.**

CemuExtend is a community fork of [Cemu](https://github.com/cemu-project/Cemu), the open-source Wii U emulator. It keeps the familiar Cemu experience while adding a built-in mod platform and extra tools for players, mod creators, and game communities.

If you only want to play Wii U games and homebrew, CemuExtend works much like Cemu. Its main difference is what it adds around the game: installable mods, permission controls, broader plugin support, and convenient connections between mods and your computer.

## How is it different from Cemu?

CemuExtend builds on Cemu rather than replacing its core experience. You still get Cemu's game compatibility, controller support, graphic packs, save management, and other familiar features.

On top of that foundation, CemuExtend adds:

- **`.cemod` packages** — a dedicated format for installing and sharing Wii U mods.
- **A built-in mod manager** — view, enable, disable, and manage mods from CemuExtend.
- **Permission prompts** — see what a mod wants to access before the game starts, then choose what to allow.
- **Support for multiple kinds of mods** — including CemuExtend-native mods and compatible WUPS plugins distributed as `.cemod` packages.
- **Better desktop interaction for mods** — mods can offer features involving keyboard and mouse input, text input, files, settings, the clipboard, windows, and screenshots when permission is granted.
- **Built-in TCPGecko compatibility** — connect supported TCPGecko clients without running separate Wii U homebrew.

These additions are designed to make advanced mods easier to install and use while keeping important choices visible to the player.

## Features

### Play with the Cemu experience you already know

Run Wii U games, homebrew, and ASM graphic packs just as you would with Cemu. Existing Cemu users should feel at home.

### Install mods as packages

Mods can be shared as a single `.cemod` file. CemuExtend finds the games each package supports and lets you manage them without manually entering title IDs.

### Stay in control of mod access

Before a mod uses protected features, CemuExtend shows a permission screen. A changed mod or a mod requesting more access can require approval again, helping you make an informed choice before launching a game.

Some mods may need broad access to the running game. Only approve packages from creators you trust.

### Use richer input and desktop features

Supported mods can make use of keyboard, mouse, text input, controller mapping, configuration, file storage, clipboard interaction, window controls, screenshots, and diagnostic information. Available features depend on the mod and the permissions you grant.

### Connect TCPGecko tools

CemuExtend includes a TCPGecko-compatible server for supported clients. It is disabled by default and can be limited to connections from your own computer.

## Related projects

The CemuExtend ecosystem is split into a few focused projects:

- **[cemod-sdk](https://github.com/CemuExtend/cemod-sdk)** — the tools used to build, check, package, and optionally sign `.cemod` files.
- **[libcemuextend](https://github.com/CemuExtend/libcemuextend)** — the library that lets a mod use CemuExtend features.
- **[cemod-example](https://github.com/CemuExtend/cemod-example)** — a complete example mod and a practical starting point for new projects.

## Project status

CemuExtend is an independent community project and is not an official Cemu release. Compatibility with mods and plugins may vary, and some features are still evolving. For ordinary Cemu questions or upstream emulator development, please visit the [official Cemu repository](https://github.com/cemu-project/Cemu).

## License

CemuExtend is licensed under the [Mozilla Public License 2.0](LICENSE.txt), following upstream Cemu. Some included components are covered by their own licenses; see the notices provided with those files.

CemuExtend is based on the work of the [Cemu project](https://github.com/cemu-project/Cemu) and its contributors.
