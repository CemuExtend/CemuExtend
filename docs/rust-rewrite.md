# CemuExtend Rust rewrite 進行台帳

最終更新: 2026-08-30（JST）

この文書は Rust rewrite の現在地、検証証拠、設計判断、レビュー対応を
逐次追記する進行台帳である。crate や Docker target が存在するだけでは機能完成と
みなさない。完了は、下記の受け入れ条件と Docker 検証ログの両方で判断する。

## 現在地

- 開発ブランチ: `rewrite/rust`
- 固定基準: `ab0b772029f0a5cd57c194cf338003fe8eae8ab8`
- 固定タグ: `rust-rewrite-baseline-ab0b7720`
- C++ oracle worktree: `../CemuExtend-cpp-oracle`。固定基準を detached HEAD で
  checkout している。
- 固定 C++ baseline build: `./docker-build-cpp-oracle.sh build` が clean な oracle
  worktree と pinned submodule revision だけから Docker context を作る。helper は存在するが、
  canonical trace、schema validation、Rust/C++ comparator の Docker target はまだない。
- 対象環境: まず Linux x86_64。Windows x86_64、macOS ARM64、Linux ARM64 は
  まだ対象外である。
- 現在の段階: baseline/oracle と headless interpreter の土台を並行して作成中。
  現在の headless 起動対象は合成 CEXH のみで、RPX loader は未実装である。
  合成 RPX/Homebrew の完全な起動、IML JIT、renderer、desktop、Pretendo、
  CEMOD/WUPS parity、移行ツールは完成していない。
- 最終統合状態: source payload fingerprint
  `sha256:23f4bd6efa7fc50a48abcb08a88b9961b9c2beedae19583a581f0c6c2329226e` で
  format-fix、UI format-fix、aggregate `ci`、release export、headless image の
  Docker build が成功した。これは現在の scaffolding の品質証拠であり、RPX boot や
  emulator parity の完成証拠ではない。
- 検証方針: build、format、lint、test、audit、release は Docker target だけで実行する。
  host 上での `cargo`、`cmake`、`ctest`、`bun` の結果を検証証拠にしない。

基準を新しい checkout に再現する場合は、タグが未作成であることを確認してから
次を実行する。

```sh
git tag rust-rewrite-baseline-ab0b7720 ab0b772029f0a5cd57c194cf338003fe8eae8ab8
git worktree add --detach ../CemuExtend-cpp-oracle rust-rewrite-baseline-ab0b7720
```

既存環境では `git rev-parse rust-rewrite-baseline-ab0b7720` と
`git -C ../CemuExtend-cpp-oracle rev-parse HEAD` が上記の完全な commit ID を返す
ことを oracle 実行前に確認する。固定タグを別 commit へ移動してはならない。

Rust toolchain は 1.97.1、`cargo-deny` は 0.20.2、`cargo-about` は 0.9.2 に固定する。
aggregate `ci` target は
Rust core の format、check、Clippy、test、dependency/advisory/license audit と、
release notice 生成 gate、React/TypeScript の contract drift、format、typecheck、
lint、test、build を実行する。
release/headless 用の Rust 依存ライセンス notice 生成には `cargo-about` 0.9.2 を
Docker 内 CLI tool として `--locked` 付きで固定導入する。`cargo-about` は workspace
依存として追加せず、Cargo.lock は変更しない。

```sh
docker build --file Dockerfile.rust --target ci \
  --build-arg RUST_VERSION=1.97.1 \
  --build-arg CARGO_DENY_VERSION=0.20.2 \
  --build-arg CARGO_ABOUT_VERSION=0.9.2 \
  --tag cemuextend-rust-ci .
```

`./docker-build-rust.sh ci` も同じ Docker-only aggregate gate である。aggregate `ci`
は `rust-builder` の生成済み license bundle を copy して依存させるため、成功時には
`cargo-about` による notice 生成と Dockerfile 内の bundle 検査も実際に通っている。
個別調査には
`rust-format`、`rust-check`、`rust-clippy`、`rust-test`、`rust-deny` target を使えるが、
個別 target の成功だけでは完了証拠にならない。依存関係を意図的に変更した際は
`./docker-build-rust.sh lock` で `Cargo.lock` を Docker から出力し、manifest
とともにレビューする。host Cargo で lockfile を生成しない。

React/TypeScript 側の Prettier 修正だけを workspace に安全反映したい場合は
`./docker-build-rust.sh ui-format-fix` を使う。この target は `ui/` だけを export し、
host Bun を使わない。

RPC generator と既存 React/TypeScript 側の drift、format、typecheck、lint、test、build
だけを個別調査する場合は次の target を使える。aggregate `ci` は `rust-ci` と
`rust-ui` の両方へ依存する。

```sh
docker build --file Dockerfile.rust --target rust-ui \
  --build-arg RUST_VERSION=1.97.1 \
  --tag cemuextend-rust-ui .
```

release artifact は host build tool を介さず次で出力する。

```sh
docker build --file Dockerfile.rust --target rust-release \
  --build-arg RUST_VERSION=1.97.1 \
  --build-arg CARGO_ABOUT_VERSION=0.9.2 \
  --output type=local,dest=result/rust .
```

`result/rust/` の配布単位は `Cemu`、`LICENSE.txt`、
`THIRD_PARTY_LICENSES.txt` の 3 ファイルである。`Cemu` だけを切り出して preview や
release として配布してはならない。Docker build は root `LICENSE.txt` をコピーし、
`crates/cemuextend` の locked Linux x86_64 dependency graph から
`THIRD_PARTY_LICENSES.txt` を生成する。生成物が空、crate attribution がない、または
license 本文らしい text がない場合、`rust-builder` は失敗する。notice template は
container/host filesystem path を出力せず、生成物に `/workspace`、`/usr/local/cargo`、
`/home/`、Windows drive path、credential-like field が混入した場合も Docker gate で失敗する。
`rust-headless` は同じ 2 つの notice file を `/usr/share/licenses/cemuextend/` に同梱する。

この preview artifact は依存 version を Cargo.lock で固定し、license policy を
`deny.toml` と `licenses/cargo-about.toml` で検査するが、Docker base image、APT source、
`cargo install` で取得する tool crate、cargo-about の upstream license text 補完経路を
digest 単位では固定していない。そのため、配布 compliance の gate には使えるが、build 全体が
bit-for-bit reproducible であるとは主張しない。公開 preview を配る場合は、対象 commit または
dirty tree の識別、実行した Docker command、生成された notice file を検証ログへ残す。

`rust-headless` は最小 runtime image である。`rust-ui` は現時点では既存 UI の静的な
品質と contract drift を検証する target であり、Rust host と CEF の接続完了は意味しない。
どの target の存在も emulator 機能の完成を意味しない。

固定 C++ baseline の通常 build は次で実行する。これは C++ reference を build/test する
helper であり、trace oracle や Rust comparator の完了証拠ではない。

```sh
./docker-build-cpp-oracle.sh build
```

## マイルストーン別チェックリスト

### 1. Baseline and oracle — 進行中

- [x] `rust-rewrite-baseline-ab0b7720` を `ab0b7720` に固定した。
- [x] C++ oracle を別の detached worktree に固定した。
- [x] 固定 revision と pinned submodule だけを export する C++ Docker build helper を追加した。
- [ ] C++ Docker build の成功証拠を Docker検証ログへ記録する。
- [ ] guest cycle を時刻とする canonical JSONL trace schema を確定する。
- [ ] 同一入力の反復結果が byte-for-byte で一致することを Docker 内で検証する。
- [ ] schema validator と C++/Rust comparator を Docker 内で通す。
- [ ] 少なくとも一つの合成プログラムを固定 C++ oracle と比較する。
- [ ] fixture manifest が許可済み metadata、digest、期待結果だけを含むことを検査する。

### 2. Headless interpreter — 進行中

- [ ] endian 型、4 GiB guest address space、権限検査を完成させる。
- [ ] PPC interpreter の必要命令と例外経路を完成させる。
- [ ] RPX/RPL loader、scheduler、VFS、最小 Cafe/IOSU を接続する。
- [x] license-clean な合成 CEXH fixture の headless 実行を Docker test で通す。
- [ ] license-clean な最小 RPX/Homebrew を決定的に終了させる。
- [ ] CPU state、memory digest、event trace を C++ oracle と一致させる。
- [ ] 不正形式、範囲外 memory、権限違反、未対応命令の negative test を通す。

### 3. IML JIT and full headless core — 未着手

- [ ] interpreter 対 IML semantic evaluator を差分検証する。
- [ ] optimizer 前後を差分検証する。
- [ ] IML 対 x86-64 code generation を差分検証する。
- [ ] invalidation、自己書換え code、例外、paired-single、NaN/denormal、
  BMI2/AVX 有無を個別検証する。
- [ ] executable memory が常に W^X であり、RWX mapping がないことを検証する。

### 4. `wgpu` renderer — 未着手

- [ ] texture reinterpretation、aliasing、render-to-texture、query、streamout の
  compatibility spike を通す。
- [ ] TV/Pad と CEF overlay composition の spike を通す。
- [ ] offscreen fixture、`wgpu` validation、shader snapshot、許容差付き frame
  comparison を通す。
- [ ] 上記完了後に限り Minecraft と Friends List の描画互換性を評価する。

### 5. Desktop, audio/input, and Pretendo — 未着手

- [ ] 既存 UI RPC の wire shape を変えず Rust/TypeScript の round-trip を通す。
- [ ] input、audio、save、account、title management を接続して検証する。
- [ ] NAS/NEX/PRUDP mock だけで offline CI を成立させる。
- [ ] ユーザー所有の非公開 fixture で Minecraft の input/audio/save/online を
  手動検証する。
- [ ] ユーザー所有の非公開 fixture で Friends List の認証、friend、presence を
  手動検証する。

### 6. CemuExtend parity and cutover — 未着手

- [ ] CEMOD package versions 1-4、CEX2、CMB1、WUPS ABI 0.7.1-0.9.1 の
  regression test を通す。
- [ ] Graphic Packs、debugger、update/title/save manager、NFC/USB portal、
  TCPGecko と全 RPC の parity を確認する。
- [ ] `inspect|copy|apply|rollback --json-report` を実装する。
- [ ] unknown XML 保持、backup、atomic write、idempotency、破損時 rollback を
  Docker fixture で検証する。
- [ ] preview が `$XDG_DATA_HOME/CemuExtend-rust-preview` だけを変更し、既存 C++
  profile を変更しないことを検証する。
- [ ] 全 gate、非公開 title gate、critical security/data-loss defect がゼロという
  条件を満たしてから stable へ昇格する。

性能は継続測定するが、互換性完成までは固定比率を release blocker にしない。
ただし対象 hardware で実用速度を維持できない場合は stable 昇格を保留する。

## 決定ログ

| 日付 | 決定 | 理由・影響 |
| --- | --- | --- |
| 2026-08-30 | `ab0b7720` を最初の互換基準として固定する | rewrite 中に oracle の期待値が動くことを防ぐ。タグは移動しない。 |
| 2026-08-30 | `rewrite/rust` と別 detached C++ worktree を使う | Rust 実装と C++ oracle を同時に比較可能にする。固定 baseline の Docker build は専用 helper から行うが、trace/comparator は別 gate とする。 |
| 2026-08-30 | build と検証は Docker target だけで行う | 開発機の toolchain 差を排除し、同じ入力で検証を再実行しやすくする。Cargo graph は lockfile で固定するが、base image、GitHub Action、APT package/source が digest 単位で固定されていないため、build 全体が bit-for-bit reproducible とは主張しない。 |
| 2026-08-30 | `cargo-deny` を 0.20.2 に固定する | 0.18.4 は現在の RustSec advisory に含まれる CVSS 4.0 を parse できなかった。失敗履歴を残した上で更新し、advisories/bans/licenses/sources の全 check を Docker 内で通した。 |
| 2026-08-30 | release/headless artifact に root license と Rust dependency notices を同梱する | 初期 Rust artifact も MPL-2.0 と依存 crate の license/notice を配布単位に含める。`cargo-about` 0.9.2 を Docker 内 CLI tool として固定し、workspace 依存や Cargo.lock には追加しない。aggregate `ci` も `rust-builder` の生成済み license bundle を copy して notice gate へ依存させる。 |
| 2026-08-30 | workspace 内 path dependency に `version = "0.1.0"` を明記する | `cargo-deny` の wildcard dependency 拒否を解消し、内部 crate の manifest version と整合させる。 |
| 2026-08-30 | Linux x86_64 を最初の platform とする | interpreter、x86-64 JIT、headless gate を先に安定させる。 |
| 2026-08-30 | C++ の旧 JIT にある RWX memory 運用を移植しない | Rust JIT は W^X を必須とし、unsafe を code cache と ABI trampoline に局所化する。 |
| 2026-08-30 | `main` の同期は milestone 間だけにする | 検証中の基準変動を避ける。milestone gate 通過後に同期し、次へ進む前に aggregate `ci` を再実行する。 |

固定 C++ oracle は最初の互換 campaign 中は更新しない。milestone gate の通過証拠を
記録した後、次の milestone の開始前に `main` を `rewrite/rust` へ merge する。
緊急 security fix の先行同期は例外としてこの台帳へ記録し、現在の gate を再実行する。
regression を消す目的で oracle の期待値を書き換えてはならない。

## 互換性上の発見

### 2026-08-30: CEMOD package version 4

初期計画の package versions 1-3 に加え、固定 baseline は
`package_version: 4` を受理し、Web UI manifest、package 内 assets、network policy
を持つ。したがって stable parity の対象は versions 1-4 である。この記載は C++
baseline の挙動についてのものであり、Rust 側の実装完了を示さない。

### 2026-08-30: JIT memory protection

C++ の旧 JIT が前提とする RWX mapping は安全性要件を満たさないため、挙動をそのまま
移植しない。Rust 実装では書き込み時と実行時を分離する W^X code cache を公開互換性を
壊さない内部実装条件とする。JIT milestone は RWX mapping がないことの検査を含む。

### 2026-08-30: dependency audit tool と advisory format

`cargo-deny` 0.18.4 は現在の RustSec advisory の CVSS 4.0 を parse できず、Docker audit
が失敗した。0.20.2 へ固定して再実行し、workspace 内 path dependency の version を
`0.1.0` と明記した後、advisories、bans、licenses、sources はすべて成功した。未遭遇の
license allowance に関する warning は残るが、audit failure ではない。0.18.4 の失敗は
検証履歴から削除しない。

### 2026-08-30: frontend response resource guard

`cex-frontend` の `MAX_RESPONSE_BYTES` は共有 RPC の baseline wire limit を変更するもの
ではない。serde が応答 JSON を解析するときに発生する追加 allocation を有限化するための
adapter-local resource guard であり、共有 `MAX_REQUEST_BYTES` を response に流用しては
ならない。実 transport の read/buffering 境界も同じ上限を先に適用し、adapter の検査を
迂回して過大な入力が serde へ到達しないようにする。

## Privacy / fixture 規則

- commercial title、system application、console dump、暗号鍵、証明書、PNID、
  access token、cookie、live Pretendo traffic を commit、Docker image、CI artifact、
  trace、log に含めない。
- repository に置く実行 fixture は生成可能、合成、license-clean なものに限る。
  private fixture は checkout 外に置く。その manifest に許可するのは不透明な
  title/version label、暗号学的 digest、期待結果だけであり、fixture 本体、鍵、
  credential、user name、machine 固有 absolute path は含めない。
- trace は allow-list 方式とし、guest cycle、正規化した event kind、公開可能な数値、
  digest だけを記録する。host environment、認証 header、token 入り URL、account data、
  path、raw guest data は記録しない。
- CI の online test は mock のみを使う。実 Pretendo はユーザーが明示実行する手動試験
  とし、log を redact し、artifact upload を無効にする。

## Docker検証ログ

| 日時（JST） | 対象 | commit/tree | 結果 | 証拠・備考 |
| --- | --- | --- | --- | --- |
| 2026-08-30 | `rust-deny` (`cargo-deny` 0.18.4) | 統合作業 tree | 失敗 | RustSec advisory の CVSS 4.0 を parse できなかった。tool version を 0.20.2 へ更新して再試行した。 |
| 2026-08-30 | `rust-deny` (`cargo-deny` 0.20.2) | 統合作業 tree | 失敗 | workspace 内 path dependency の wildcard version を拒否した。root manifest の各内部 dependency に `version = "0.1.0"` を追加して再試行した。 |
| 2026-08-30 | `rust-clippy` | 統合作業 tree | 成功 | workspace/all-targets の Clippy が成功した。 |
| 2026-08-30 | `rust-test` | 統合作業 tree | 成功 | `cargo test --workspace --all-targets --locked` が成功。Rust test は合計 106 件。 |
| 2026-08-30 | `rust-ui` | 統合作業 tree | 成功 | generator drift、Prettier、TypeScript compiler、ESLint、27 tests、Vite build がすべて成功した。 |
| 2026-08-30 | `rust-deny` (`cargo-deny` 0.20.2) | 統合作業 tree | 成功 | advisories、bans、licenses、sources がすべて成功。未遭遇 license allowance の warning は残るが failure ではない。 |
| 2026-08-30 | `rust-release` | 統合作業 tree | 成功 | Docker local output で `result/rust/Cemu` の export に成功した。 |
| 2026-08-30 | `rust-headless` | 統合作業 tree | 成功 | headless runtime image の生成に成功した。 |
| 2026-08-30 | aggregate `ci` | 統合作業 tree | 成功 | Rust format/check/Clippy/test/audit と UI contract/check/test/build を含む aggregate target が成功した。 |
| 2026-08-30 | C++ oracle `build` | `ab0b7720` | 未完了 | clean な pinned context の生成までは成功したが、Docker/BuildKit が停止状態になり image は生成されなかった。C++ oracle build は未検証のまま。 |

### 最終 dirty-tree 証跡

最終 source payload fingerprint は
`sha256:23f4bd6efa7fc50a48abcb08a88b9961b9c2beedae19583a581f0c6c2329226e`
である。自己参照を避けるため本台帳 `docs/rust-rewrite.md` を明示的に除外し、`result/`
など Git が ignore する build artifact も入力に含めない。tracked binary diff と path 順に
sort した untracked file の SHA-256 を次の command で hash した値である。

```sh
{
  git diff --binary HEAD -- . ':(exclude)docs/rust-rewrite.md'
  git ls-files --others --exclude-standard -z -- . \
    ':(exclude)docs/rust-rewrite.md' | sort -z | xargs -0 -r sha256sum
} | sha256sum
```

以下の wrapper と展開後 command はすべて、この同一 source payload fingerprint を
対象にした。

```sh
./docker-build-rust.sh format-fix
./docker-build-rust.sh ui-format-fix
./docker-build-rust.sh ci
./docker-build-rust.sh release
./docker-build-rust.sh headless

docker build --file Dockerfile.rust --progress plain --target rust-format-fix \
  --build-arg RUST_VERSION=1.97.1 \
  --build-arg CARGO_DENY_VERSION=0.20.2 \
  --build-arg CARGO_ABOUT_VERSION=0.9.2 \
  --output type=local,dest=<mktemp> .
docker build --file Dockerfile.rust --progress plain --target rust-ui-format-fix \
  --build-arg RUST_VERSION=1.97.1 \
  --build-arg CARGO_DENY_VERSION=0.20.2 \
  --build-arg CARGO_ABOUT_VERSION=0.9.2 \
  --output type=local,dest=<mktemp> .
docker build --file Dockerfile.rust --progress plain --target ci \
  --build-arg RUST_VERSION=1.97.1 \
  --build-arg CARGO_DENY_VERSION=0.20.2 \
  --build-arg CARGO_ABOUT_VERSION=0.9.2 \
  --tag cemu-extend-rust:ci .
docker build --file Dockerfile.rust --progress plain --target rust-release \
  --build-arg RUST_VERSION=1.97.1 \
  --build-arg CARGO_DENY_VERSION=0.20.2 \
  --build-arg CARGO_ABOUT_VERSION=0.9.2 \
  --output type=local,dest=result/rust .
docker build --file Dockerfile.rust --progress plain --target rust-headless \
  --build-arg RUST_VERSION=1.97.1 \
  --build-arg CARGO_DENY_VERSION=0.20.2 \
  --build-arg CARGO_ABOUT_VERSION=0.9.2 \
  --tag cemu-extend-rust:headless .
```

上記2つの `<mktemp>` は helper が実行時に作る別々の一時 directory を表す。helper は
local output を export した後、整形済み source を作業 tree へ copy し、一時 directory を
安全に破棄する。format-fix target は image tag を作らない。

| 日時（JST） | 対象 | fingerprint | 結果 | 証拠・備考 |
| --- | --- | --- | --- | --- |
| 2026-08-30 | `rust-format-fix` | `23f4bd6e…9226e` | 成功 | Rust source の Docker-only format fix target が成功した。 |
| 2026-08-30 | `rust-ui-format-fix` | `23f4bd6e…9226e` | 成功 | UI source の Docker-only format fix target が成功した。 |
| 2026-08-30 | `rust-clippy`（final前） | final前の統合作業 tree | 失敗 | VFS documentation、GPU、CEMOD、migrate test の lint error を検出し、修正後に aggregate `ci` で再検証した。 |
| 2026-08-30 | `cargo-about` 0.9.2（final前） | final前の統合作業 tree | 失敗 | default install に CLI feature がなく command が存在しなかった。`--features cli` を追加して再検証した。 |
| 2026-08-30 | aggregate `ci` | `23f4bd6e…9226e` | 成功 | Rust fmt/check/Clippy、workspace 139 tests、cargo-deny advisories/bans/licenses/sources、RPC drift、Prettier/tsc/ESLint、UI 30 tests、Vite、release build、cargo-about notice gate がすべて成功した。 |
| 2026-08-30 | `rust-release` | `23f4bd6e…9226e` | 成功 | `result/rust/Cemu` 741008 bytes、`LICENSE.txt` 16725 bytes、`THIRD_PARTY_LICENSES.txt` 94701 bytes を export した。 |
| 2026-08-30 | `rust-headless` | `23f4bd6e…9226e` | 成功 | non-root runtime と license notices を含む headless image の生成に成功した。 |

途中で2つの subagent が Docker-build-only 指示に反して `docker run` test を実行した。
その結果は正式証拠から除外した。同じ対象は上記 aggregate `ci` の Docker build 内で
再検証済みであり、最終判定には Docker build の結果だけを使用した。

過去行の Rust 106 tests / UI 27 tests は当時の履歴として残す。最終件数と判定は同じ
fingerprint に結び付いた上表の Rust 139 tests / UI 30 tests を参照する。C++ oracle は
clean context 生成後の BuildKit hang が未解決であり、image と C++ test は未検証である。

検証時は実行した完全な `docker build` command、対象 commit または dirty tree の識別、
成否、失敗した target/test、必要な log の保存先をこの表へ追記する。秘密情報を含む log
は保存せず、redact した要約だけを記録する。

## レビュー指摘と対応

| 日時（JST） | レビュー | 指摘 | 対応 | 状態 |
| --- | --- | --- | --- | --- |
| 2026-08-30 | architecture review | `rust-ci` に dependency/advisory/license audit がなく「完全な gate」と呼べない。 | `rust-deny` を `rust-ci` の祖先へ追加し、aggregate `ci` を正式 gate に変更。0.18.4 の CVSS 4.0 parse failure 後、0.20.2 へ固定して全 audit を通した。 | 対応済み・Docker 確認済み |
| 2026-08-30 | architecture review | RPC manifest 変更時に Rust/TS generated contract drift を検知する必要がある。 | Rust 側 golden test を `rust-test` に追加し、generator drift と UI checks を行う `rust-ui` を aggregate `ci` へ接続。 | 対応済み・Docker 確認済み |
| 2026-08-30 | architecture review | oracle/comparator 用 Docker target と証拠場所がまだない。 | 固定 C++ build helper と clean context export は追加済みだが、BuildKit hang により image 未生成。canonical trace、schema validator、Rust/C++ comparator も未完了 gate のまま。 | 一部対応・未検証 |
| 2026-08-30 | architecture review | CI path filter が generator と generated files を含んでいない。 | contract、generator、generated Rust/TS、本台帳に加え、`.dockerignore`、`compat/**`、`ui/**`、root `LICENSE.txt`、`licenses/**` を trigger 対象へ追加。凍結した C++ oracle header は生成対象に含めない。 | 対応済み・静的確認済み |
| 2026-08-30 | architecture review | action/base image の可変 tag に対して再現性の表現が広すぎる。 | 再現性の主張を locked Cargo graph と同一入力の再実行性に限定。digest pin は未対応事項として残す。 | 文書対応済み |
| 2026-08-30 | safety review | 初期実装の指摘は未回収。 | review を回収し、以下の個別指摘へ展開した。 | 回収済み |
| 2026-08-30 | safety review | headless trace output preservation と code-size validation が未完了。 | headless trace 出力保持と code-size 検査を追加し、最終 source payload fingerprint の workspace 139 tests と headless image build を通した。 | 対応済み・Docker 確認済み |
| 2026-08-30 | safety review | Docker secret exclusions が未確認。 | Docker context/image の secret exclusion を反映し、最終 source payload fingerprint の aggregate `ci`、release export、non-root headless image build を通した。 | 対応済み・Docker 確認済み |
| 2026-08-30 | safety review | trace semantics と integration compile drift の検証が未完了。 | canonical trace semantics と cross-crate integration checks を追加し、最終 source payload fingerprint の workspace 139 tests と aggregate `ci` を通した。 | 対応済み・Docker 確認済み |
| 2026-08-30 | final integration review | Clippy が VFS documentation、GPU、CEMOD、migrate test の lint error を検出した。 | 各 error を修正し、最終 source payload fingerprint の Clippy と aggregate `ci` を通した。失敗行は Docker検証ログに保持した。 | 対応済み・Docker 確認済み |
| 2026-08-30 | final integration review | `cargo-about` 0.9.2 を default feature で install すると CLI command が存在しない。 | install に `--features cli` を追加し、release notices と aggregate notice gate を通した。 | 対応済み・Docker 確認済み |
| 2026-08-30 | safety review | release/headless に license bundle と non-root runtime の最終確認が必要。 | source payload fingerprint `23f4bd6e…9226e` の aggregate `ci` notice gate、`rust-release` の `LICENSE.txt` / `THIRD_PARTY_LICENSES.txt` export、non-root `rust-headless` image をすべて Docker build で確認した。 | 対応済み・Docker 確認済み |
| 2026-08-30 | process review | 2 subagent が Docker-build-only 制約に反して `docker run` test を実行した。 | 当該結果を正式証拠から除外し、同じ対象を最終 aggregate `ci` の Docker build で再検証した。 | 証拠是正済み |
| 2026-08-30 | safety review | `rust-builder`、`rust-headless`、`rust-release` が `Cemu` binary だけを出力し、root license と Rust dependency notice を同梱していない。 | `cargo-about` 0.9.2 を Docker 内 CLI tool として固定し、root `LICENSE.txt` と `THIRD_PARTY_LICENSES.txt` を `rust-builder` で生成・検査する。aggregate `ci` は生成済み bundle を copy して notice gate を必須化する。notice は filesystem path を出力せず、path/credential-like field 混入も Docker gate で拒否する。`rust-headless` は `/usr/share/licenses/cemuextend/`、`rust-release` は `result/rust/` へ同梱する。 | 対応済み・Docker 確認済み |

レビューで gate を満たさないと判明した項目は、完了 checkbox を未完了へ戻す。対応を
実装しただけでは解決済みにせず、Docker-only 再検証の行をリンクして閉じる。

## 次の作業

1. C++ oracle build の Docker/BuildKit hang を切り分け、固定 `ab0b7720` image の生成と
   C++ test 成功証拠を Docker検証ログへ追加する。
2. canonical JSONL schema、validator、Rust/C++ comparator 用 Docker target を完成させ、
   最小合成 program の oracle 比較を行う。
3. CEXH synthetic path に留まっている headless loader を RPX/RPL へ拡張し、
   license-clean な最小 RPX/Homebrew の決定的終了を検証する。
4. Docker-only aggregate `ci`、release、headless の成功を今後の変更でも維持し、失敗と
   再試行をこの台帳へ逐次追記する。
5. milestone 1 と 2 の全 gate が通るまでは IML JIT と renderer の完成を主張しない。
