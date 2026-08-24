# ゲームWindow終了後の再起動クラッシュ — 引き継ぎ

更新日時: 2026-08-24 20:39 JST

## 依頼内容

- ゲーム画面をランチャーとは別Windowで動作させる。
- ゲームWindowを閉じてもランチャーは動作を継続する。
- ランチャーを閉じても実行中のゲームは動作を継続する。
- ゲームWindowにはRuntime Overlayを表示する。
- ユーザーはC++側を自分でコンパイルする。こちらではWeb UIのみビルドする。

## 再現手順

1. Minecraft: Wii U Editionを起動する。
2. Cached pipelineの読み込みが一定の割合で停止している状態にする。
3. ゲームWindowへフォーカスを置き、`Super+Q` で閉じる。
4. ランチャーから同じゲームを再度起動する。
5. 2回目の起動直後にCemuプロセスがSIGSEGVで終了する。

## OS側のclose操作

- Desktop: Hyprland 0.56.1
- `hyprctl binds -j` の実行結果:
  - key: `Q`
  - modmask: `64` (`SUPER`)
  - dispatcher: `killactive`
- CemuはXWayland側で動作している。

## 最新再現時のファイル時刻

- 実行バイナリ: `bin/.Cemu_release.bin`
  - 更新日時: `2026-08-24 20:32:45 +0900`
  - サイズ: `36611360 bytes`
- ログ: `/home/umi/.local/share/Cemu/log.txt`
  - 更新日時: `2026-08-24 20:33:51.648030481 +0900`
  - サイズ: `26819 bytes`

## 最新ログの時系列

```text
[20:10:12.558] Mounting title 00050000101dbe00
[20:10:13.546] ------- Run title -------
[20:10:17.902] IOSU_FPD: Created friend server session
[20:10:17.902] NEX: Attempt async friend service login
[20:10:33.588] ------- Stop title -------
[20:10:33.745] ------- Title stopped cleanly -------
[20:10:35.049] Mounting title 00050000101dbe00
[20:10:35.844] ------- Run title -------
[20:10:36.178] Shader cache entry 17 invalid, deleting...
[20:10:37.222] _IPCDispatchToResourceManager(): Resource manager destroyed before all IPC commands were processed
[20:10:37.222] Error occurred while trying to dispatch IPC
Error: signal 11
```

最新クラッシュ時のPPC情報:

```text
IP 0x03454528
LR 0x0340f2e8
Thread 0x0e1a3e40
```

ホスト側スタックに表示された実行アドレス:

```text
[0x73d207cd3497]
libjavascriptcoregtk-4.1.so.0(+0x19440f7)
libc.so.6(+0x422a0)
[0x73d207cd3497]
```

## 確認済みの事実

- 最新バイナリには `Stop title` と `Title stopped cleanly` の診断行が含まれている。
- `Super+Q` 後、1回目のタイトル終了は `Title stopped cleanly` まで到達している。
- `Title stopped cleanly` の約1.3秒後に2回目のMountが開始している。
- 2回目の `Run title` の約1.38秒後にIPCエラーとSIGSEGVが発生している。
- 最新ログには `IOSU-Kernel: abandoned N delayed IPC command(s)` は出ていない。
- 最新クラッシュについて新しいsystemd coredumpは保存されていない。
- `coredumpctl` に表示されるCemuのcoreは2026-08-11の古いSIGABRTのみ。
- Mii/FFL初期化の `OSPanic` はログに出るが、ユーザーから「以前から存在する別問題」と明示されている。
- 直近の `npm run build:embedded` は成功している。
- C++ビルドと実機再現はユーザー側で実施している。

## 現在入っている主な変更

### 別ゲームWindowとOverlay

- Linux/Windows/macOS用の独立したゲーム描画Window。
- Launcher WindowとGame Windowの寿命分離。
- TV/GamePad用Runtime Overlay WebView。
- ゲームWindowのclose要求を `EmulationController::Stop()` へ接続。
- close要求の重複抑制とMainWindow generation検証。

関連ファイル:

- `src/webview/NativeWindowHost.h`
- `src/webview/NativeWindowHostGtk.cpp`
- `src/webview/NativeWindowHostWin.cpp`
- `src/webview/NativeWindowHostCocoa.mm`
- `src/webview/WebFrontend.cpp`
- `ui/src/features/runtime-overlay/RuntimeOverlay.tsx`
- `ui/src/styles/runtime-overlay.css`

### タイトル終了順序

`CafeSystem::ShutdownTitle()` で以下の順序へ変更済み:

1. タイトル停止要求
2. Scheduler終了
3. Title thread join
4. IOSU title services停止
5. Latte停止
6. PPC thread削除
7. CEMod/RPL解放
8. Recompiler・mount・memory解放
9. `Title stopped cleanly` を出力

関連ファイル:

- `src/Cafe/CafeSystem.cpp`

### NEX/FPD/IPC終了処理

- NEX serviceへ `destroyAndWait()` を追加。
- 終了時にpending callbackを呼ばないモードを追加。
- NEX login threadをdetachからjoin可能なthreadへ変更。
- FPD service worker停止後、NEX停止完了を待ってからFPD delayed requestを解放。
- IOSU dispatch commandへdispatch queue IDを保持。
- Message queue破棄時に、そのqueueに属する未完了dispatch commandを解放。

関連ファイル:

- `src/Cemu/nex/nex.cpp`
- `src/Cemu/nex/nex.h`
- `src/Cemu/nex/nexFriends.cpp`
- `src/Cemu/nex/nexFriends.h`
- `src/Cemu/nex/nexThread.cpp`
- `src/Cafe/IOSU/kernel/iosu_kernel.cpp`
- `src/Cafe/IOSU/kernel/iosu_kernel.h`
- `src/Cafe/IOSU/legacy/iosu_fpd.cpp`
- `src/Cafe/IOSU/nn/iosu_nn_service.cpp`
- `src/Cafe/IOSU/nn/iosu_nn_service.h`

### Vulkan pipeline cache終了処理

- Pipeline compiler threadのdetachを廃止。
- `EndLoading()` と `Close()` でcompiler threadをjoin。
- Pipeline cache writerをjoin。
- 二重の非同期FileCache writeを同期writeへ変更。

関連ファイル:

- `src/Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineStableCache.cpp`
- `src/Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineStableCache.h`

## 現在の状態

- ゲームWindowのclose要求からタイトル終了完了ログまでは到達する。
- 20:10ビルドでは、その後の同一タイトル再起動で同じIPCエラーとSIGSEGVが再現した。
- 20:32ビルドでも再現したため、20:24修正だけでは未解決だった。
- 追加診断により、2回目のタイトルが1回目の終了済みIOS handle `0x0001100f`で`IOS_IOCTL`を送っていることを確認した。
- 20:39時点で、FSA/CCR-NFCと全title-owned IOS handleの再初期化を追加済み。C++再ビルド後の実機確認は未実施。
- 作業ツリーは未コミットで、上記以外のWeb UI変更も同じ作業ツリーに含まれている。

## 20:24追加修正（要C++再ビルド・再現確認）

### IPC dispatch slotのABA再利用防止

- タイトル終了時に破棄した遅延IPC commandをfree listへ即時返却せず、retired状態にする。
- IOSU側の全producer停止後、次タイトルの`TitleStart`先頭でretired slotを回収する。
- 破棄済み／応答中commandへの遅延`IOS_ResourceReply`を拒否する。
- dispatch pool枯渇時のnull dereferenceを避け、IOS errorとしてguestへ返す。
- stale handle診断へhandle値、command ID、判明しているdevice pathを追加する。

関連ファイル:

- `src/Cafe/IOSU/kernel/iosu_kernel.cpp`
- `src/Cafe/IOSU/kernel/iosu_kernel.h`

### タイトル単位IPC serviceの完全リセット

- `IPCSimpleService`に加えて`IPCService`もworker join後にmessage queueとresource managerを破棄する。
- 次タイトルで`/dev/acp_main`と`/dev/boss`が新しいqueueへ確実に登録されるようにする。

関連ファイル:

- `src/Cafe/IOSU/nn/iosu_nn_service.cpp`

### nn_fp host stateのRPL寿命への同期

- `nn_fp`のRPL map時に`/dev/fpd` handleを明示的に初期化する。
- IOSU FPD停止・全PPC thread削除後にFPD handle、callback参照、host IPC heapを破棄する。
- `/dev/fpd`のopen失敗を成功扱いせず、エラーを返す。
- `Finalize`後に古いFPD handleを残さない。

関連ファイル:

- `src/Cafe/OS/libs/nn_fp/nn_fp.cpp`
- `src/Cafe/OS/libs/nn_fp/nn_fp.h`
- `src/Cafe/CafeSystem.cpp`

### 検証済み項目

- `git diff --check`: 成功
- `npm run build:embedded`: 成功（Viteのchunk size warningのみ）
- C++ビルド・Minecraftでの再現確認: ユーザー側で実施予定

## 20:32再現結果と20:39追加修正（要C++再ビルド・再現確認）

20:32ビルドで得られた決定的な診断行:

```text
[20:33:51.648] _IPCDispatchToResourceManager(): Resource manager for handle 0x0001100f was destroyed before command 6 was processed
[20:33:51.648] Error occurred while trying to dispatch IPC
Error: signal 11
```

`command 6`は`IOS_IOCTL`。遅延IPC replyの再利用ではなく、Cafe OS HLE側の静的client状態が前タイトルのclosed handleを保持していた。

### FSAをtitle-owned serviceへ変更

- `/dev/fsa` worker、message queue、resource managerをタイトルごとに開始・停止する。
- 終了時にworkerをjoinしてqueueを破棄する。
- FSA client、file handle、directory handleをタイトル境界で解放する。
- coreinit再map時にFSA IPC buffer pool、active client一覧、初期化フラグをリセットする。

関連ファイル:

- `src/Cafe/CafeSystem.cpp`
- `src/Cafe/IOSU/fsa/iosu_fsa.cpp`
- `src/Cafe/OS/libs/coreinit/coreinit_FS.cpp`

### CCR-NFC/NTAGをtitle lifetimeへ同期

- `/dev/ccr_nfc` workerとresource managerをSystem lifetimeからTitle lifetimeへ変更する。
- NTAGのcached CCR-NFC handleとguest callback参照をRPL map時にリセットする。
- CCR-NFC handleを一度だけopenし、open成功時に`ccrNfcOpened`を正しく設定する。

関連ファイル:

- `src/Cafe/IOSU/ccr_nfc/iosu_ccr_nfc.cpp`
- `src/Cafe/OS/libs/ntag/ntag.cpp`

### IOS handleのtitle境界リセット

- Resource Manager自体がSystem lifetimeでも、guestから発行された全IOS handleはTitle lifetimeとして終了時に無効化する。
- 残ったdispatch commandもretired状態へ移し、次タイトル開始時に回収する。

関連ファイル:

- `src/Cafe/IOSU/kernel/iosu_kernel.cpp`

## 確認用コマンド

```bash
stat -c '%y %s %n' /home/umi/.local/share/Cemu/log.txt bin/.Cemu_release.bin
rg -n "Stop title|Title stopped cleanly|abandoned .*IPC|reclaimed .*IPC|Mounting title|Run title|signal 11|Resource manager.*destroyed|Stale handle|Invalid handle" /home/umi/.local/share/Cemu/log.txt
git status --short
git diff --check
```
