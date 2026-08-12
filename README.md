# CemuExtend

CemuExtendは、Wii Uエミュレーター[Cemu](https://github.com/cemu-project/Cemu)をベースに、`.cemod`パッケージとCemuExtend ABI 2（CEX2）を追加したフォークです。通常のCemuと同じWii Uタイトル・Homebrew・ASM Graphic Packを実行できます。

`.cemod`には次の2つの実行モードがあります。

| モード | 用途 | 信頼境界 |
| --- | --- | --- |
| `isolated` | CEX2サービスだけを使う第三者Mod | Mod専用PPCアドレス空間、W^X、CPU・memory quota |
| `trusted_native` | Minecraft hook、GX2/Cafe API、ゲームmemoryを使うnative Mod | sandboxなし。同一タイトルのnative Modは相互に信頼 |

どちらも`package_version: 1`、`api_version: 2`、`execution_mode`を持つ`manifest.json`が必須です。旧共有memory ABI 1は受け付けません。

## CEX2

CEX2は`CEX2Query`、`CEX2Open`、`CEX2Submit`、`CEX2Poll`、`CEX2Cancel`、`CEX2Close`で構成されます。requestは呼び出し時にCemu所有memoryへコピーされ、responseはPoll時に検査済みguest bufferへコピーされます。共有Header、Ring、State Page、Bulk Poolはありません。1 messageの上限は64 KiBです。

提供サービスはCore、Input、Logging、Configuration、File、Clipboard、Window、Capture、Diagnosticsです。FileとConfigurationはownerごとのnamespaceを使用し、path・quota・型・pagination・async completionをhost側で検証します。

CEX2 ABI 2.1のInput／Windowサービスは、USB HID keyboard、UTF-32 text、通常mouseとWindows Raw Inputによる相対mouse移動を配信します。guestはpointer policyでOS cursorの表示、absolute GUI操作、captured relative視点操作、Raw Mouseの優先／無効化を切り替えられます。focusを失った場合はhostが押下状態を解放し、pointer policyを一時的にDefaultへ戻します。

mapped VPAD入力では`MappedInputFlag::ReplacePhysical`を指定できます。有効な注入が存在する間、通常のCemu controller profileから来るボタンと左右stickだけを消去してからmapped入力を適用し、touch・gyroなどのsensor状態は保持します。これにより、Mod独自のkeyboard/mouse割り当てと既存profileの二重入力を防止できます。

`isolated` ownerはMod principal単位です。`trusted_native` ownerは`trusted-title:<titleId>`であり、同じタイトルの承認済みnative ModはCEX2 session、権限、File／Configuration namespaceを共有します。

## `.cemod` package

`.cemod`はZIP containerで、`manifest.json`と正確に1つのpayload、任意のEd25519公開鍵と署名を格納します。package version 1は従来どおりPPC32 big-endian ELF (`mod.elf`)を暗黙に選択します。package version 2は`payload` descriptorで`cemod_elf/mod.elf`または`wups/plugin.wps`を選択し、WUPSは`trusted_native`だけで許可します。署名対象は各entryの名前・長さ・SHA-256を固定順に連結したdigestです。

WPSはRPL section、CRC/FILEINFO、圧縮、symbol/relocation、`.wups.meta`、`.wups.hooks`、`.wups.load`を実行前にhost側で厳格にinspectionします。検証済みpluginは外部RPLとしてロードされ、owner/generation付きlifecycle、WUMS module graph/export、Cafe RPL/HLE優先のimport解決、transactional FunctionPatcher（named/fixed address、far trampoline、`real_*`、dynamic RPL追従）へ接続されます。Storage／Config等のbackend serviceと標準Aroma moduleの提供範囲を含む実装状況・明示的な非対応箇所・license境界は[`docs/wups-support-design.md`](docs/wups-support-design.md)に記載しています。

署名済みprincipalはpublisher fingerprintとMod ID、未署名principalはpackage SHA-256です。未署名packageは内容が変わると再承認が必要です。署名済み更新も要求権限が増えた場合は再承認が必要です。署名だけで権限が増えることはありません。

packageはユーザーデータの`cemuextend/mods/`へ配置します。`Options` → `General settings` → `CemuExtend`を開くと、manifestに含まれる対象ゲームが自動で一覧化されます。Mod名のチェックを付けると有効、外すと無効になり、変更は次回タイトル起動時に反映されます。Title IDの手入力は不要です。

Mod単位の権限パネルにある「Trust future updates to this Mod」はopt-inの例外です。有効にすると、その時点で承認した権限をMod ID単位で記録し、以後principalが変わっても要求権限が承認範囲を超えない限り再承認なしで自動的に引き継がれます。要求権限が増えた場合は従来どおり再承認が必要です。デフォルトは無効(未チェック)で、principalごとの再承認という既定の挙動は変わりません。

有効なModに未許可または新規要求の権限がある場合、CemuExtendはタイトルのマウント前に権限確認画面を表示します。複数Modは1画面にまとめられ、選択した権限を保存してからゲームを起動します。権限を一部拒否したまま起動することもできますが、不足が残る間は次回起動時にも確認されます。キャンセルした場合、ゲームは準備・起動されません。

### `isolated`

`isolated` ELFは`cemod_init`、`cemod_tick`、`cemod_event`、`cemod_shutdown`を公開します。codeはRX、data／stackはRW-NXです。ゲームmemory、任意Cafe API、任意hookは公開されません。InterpreterとJITの双方でaddress spaceと権限を検査します。

### `trusted_native`

`trusted_native`はPPC32 big-endian ET_DYNです。undefined symbolとW+X segmentを拒否し、対応relocationは`R_PPC_NONE`、`ADDR32`、`ADDR16_LO`、`ADDR16_HI`、`ADDR16_HA`、`REL24`、`REL32`、`RELATIVE`だけです。Cafe、GX2、CEX2はMod自身が`OSDynLoad`で解決します。

最初のbranchだけをELFの`.cemod.bootstrap` sectionにCMB1 tableとして記録します。Cemuはmodule hash、対象命令とmask、handlerの実行segment、REL24範囲、patch競合を検証してから一括適用し、JIT cacheを無効化します。それ以降のhookはMod側C++と`libhookevent`で設置できます。trusted payloadの個別unloadは実行中タイトルでは拒否されます。

trusted ELFはASM適用後に`mod_id`順で共有8 MiB codecaveへ配置されます。検証失敗、競合、容量不足では該当Modをロードせず、部分patchを残しません。タイトル終了時は先にguest callbackとCEX2 sessionを停止し、`OSSchedulerEnd`と全PPC thread削除が完了したlate phaseでだけ元命令を復元してJITを再無効化し、codecaveを解放します。late phase未完了のまま次タイトルを準備することはできません。

> [!WARNING]
> `trusted_native`はsandboxではありません。ゲームmemory、Minecraft allocator、GX2/Cafe API、他のnative Modへ無制限にアクセスできます。内容とpublisherを信頼できるpackageだけを承認してください。

## SDK

ゲストSDKは[libcemuextend](https://github.com/PinkDiamondTeam/libcemuextend)です。CEX2 Client APIとwire schemaは両モード共通です。`isolated`は検査済みHLE stub、`trusted_native`は`ConfigureTrustedCafePlatform`で設定する`OSDynLoad` resolverを使用します。

Minecraft向けの参照実装は隣接する`mcwiiu-client-template`にあり、描画hook、Minecraft allocator、ImGui、GX2 backend、`libhookevent`、CEX2 Clientを組み合わせた署名可能なtrusted `.cemod`をDockerで生成します。

## TCPGecko

CemuExtendには[tcpgecko](https://github.com/BullyWiiPlaza/tcpgecko)互換の内蔵serverがあります。実機ではWii U本体で動くhomebrewが必要ですが、CemuExtend自体がgame memoryへ直接accessできるため、同じnetwork protocolをホスト側(Cemu本体)で実装しています。動作確認しているclientはIranjin/ZaqroUのみです。

- `Options` → `General settings` → `TCPGecko`で有効化・port番号(既定7331)・LAN許可・code handler versionを設定します。既定は無効、有効時もloopbackのみで、LAN許可は別途必要です。
- Code handlerはtcpgecko由来の実PPC binary(`General`/`Latest`)をtitleごとにmemory上へ展開し、毎frame実行します。既存のGecko/Ocarina形式cheat codeとの互換性を保つためです。
- code送信・client接続/切断時はnotification overlayにログが表示されます。

`src/Cafe/HW/Espresso/TcpGecko/`はtcpgeckoの実装を移植しているため、CemuExtend全体のMPL-2.0ではなく[GPLv3](src/Cafe/HW/Espresso/TcpGecko/LICENSE)です(`dependencies/`と同様、file単位の別license)。

## Docker build

サブモジュールを取得後、次を実行します。

```sh
git submodule update --init --recursive
./docker-build.sh
```

scriptはRelease buildとCTestを実行し、両方が成功した場合だけ最終実行ファイルを次の固定先へ原子的に配置します。

```text
CemuExtend/result/bin/Cemu_release
```

一般的な依存関係と非Docker buildは[BUILD.md](BUILD.md)を参照してください。

## 主な実装

| パス | 内容 |
| --- | --- |
| `src/Cafe/HW/Espresso/CemodPackage*` | ZIP、manifest、署名、ELF検証 |
| `src/Cafe/HW/Espresso/ModExecutionContext*` | isolated memory・CPU・principal |
| `src/Cafe/HW/Espresso/TrustedCemodRuntime*` | ET_DYN relocation、CMB1 bootstrap、codecave lifecycle |
| `src/Cafe/OS/libs/cemuextend/` | CEX2 owner、session、service、storage |
| `src/gui/wxgui/GeneralSettings2.cpp` | package承認、実行モード・署名・権限表示 |
| `src/gui/wxgui/CemodPermissionDialog.cpp` | ゲーム起動前のMod権限確認 |
| `dependencies/libcemuextend/` | ABI 2 SDKとwire schema |
| `src/Cafe/HW/Espresso/TcpGecko/` | TCPGecko互換server、code handler、guest job marshaling (GPLv3) |

## License

CemuExtendはupstream Cemuと同じ[Mozilla Public License 2.0](LICENSE.txt)でライセンスされます。`dependencies/`と一部のsourceには各ファイル記載の別licenseが適用されます。
