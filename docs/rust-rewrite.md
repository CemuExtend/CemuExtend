# CemuExtend Rust rewrite 進行台帳

最終更新: 2026-08-31（JST）

この文書は Rust rewrite の現在地、検証証拠、設計判断、レビュー対応を
逐次追記する進行台帳である。crate や Docker target が存在するだけでは機能完成と
みなさない。完了は、下記の受け入れ条件と Docker 検証ログの両方で判断する。

## 現在地

- 開発ブランチ: `rewrite/rust`
- 固定基準: `984dd5e786546e7803a6ce879af2a1352f166b38`
- 固定タグ: `rust-rewrite-baseline-ab0b7720`
- C++ oracle worktree: `../CemuExtend-cpp-oracle`。固定基準を detached HEAD で
  checkout している。
- 固定 C++ baseline build: `./docker-build-cpp-oracle.sh build` が clean な oracle
  worktree と pinned submodule revision だけから Docker context を作る。helper は存在するが、
  pinned Gitlink history hardening後の`dev`、compile/link 877/877、CTest 37/37は固定
  `ab0b7720`で成功した。post-test runtime bundleとbuild image、canonical C++/Rust trace
  parityは未完成である。
- 対象環境: まず Linux x86_64。Windows x86_64、macOS ARM64、Linux ARM64 は
  まだ対象外である。
- 現在の段階: baseline/oracle と headless interpreter の土台を並行して作成中。
  fixed synthetic main RPX1 + provider RPL1 の positive link-state と near REL24 call execution
  を Docker 検証済みである。
  retail RPX/RPL、import、relocation、compression、Cafe OS、Homebrew の完全な
  起動、IML JIT、renderer、desktop、Pretendo、
  CEMOD/WUPS parity、移行ツールは完成していない。
- 最終統合状態: source payload fingerprint
  `sha256:e20c0843060bde8656512a1119ea0efe1ff4041a77c14384456230d5e5498dd9`
  （本台帳自身とresultを除外）で、main session が aggregate `ci`、combined artifact export、headless
  imageを成功させた。これは fixed synthetic main RPX1 + provider RPL1 のpositive link-stateとRust-only
  near REL24 call executionの品質証拠であり、public loader/full lifecycle、retail/general relocation、MMU permission parity、C++ CPU parityの完成証拠ではない。
- RPX/RPL milestone 現在地: main raw `0x48000001`→linked `0x48002001`、provider ADDR32 site
  `0x10002008`→`0x02002000`、main REL24 site `0x02000000` displacement 8192、`bl → addi r3,42 → blr → stop`、
  final r3=42/LR `0x02000004`/PC `0x02000008`/cycles-retired 4を検証済み。linked 5 pages/20480、executed 21 pages/86016。
  far trampoline、REL14、ADDR16、general parityは未実装・未完了である。
- Oracle milestone 現在地: `cex-trace-compare` と `rust-oracle-smoke` はRust側の
  match/mismatch/error/no-clobber自己整合性をDocker確認済み。固定 C++ adapterの
  `rpl-call-trace`、`rpl-link-trace`、`trace` targetはproduction loader/link-stateの5-record
  exact比較をDocker確認済みである。`compat/golden` manifestはoracle commitとadapter/CMake
  source hashをpinし、aggregate `ci`はold/new goldenとRust-only execution goldenを比較する。
  C++ oracleはCPU実行およびMMU permission parityを証明せず、retail/general trace parityでもない。
- Parse/map contract現在地: Rust `cex-rpx-fixture` / `cex-rpx-contract-trace`が生成する
  412-byte fixtureとcanonical 3-record traceを、固定`ab0b7720`のtest-only C++ adapterが
  `RPLLoader_ValidateExternalImage`とmapping policyで処理し、Rust comparator exit 0で
  exact equalityを確認した。Cafe/MMU/CPU executionやmalformed corpus parityではない。
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

### C++ oracle 診断 target とログ

helper の診断 target は引数から Docker target へ明示的に次のように対応する。
`base` → `cemu-extend-base`、`dev` → `dev`、`build` → `build`、`win` →
`build-windows-artifact`。引数を省略した場合は `build` である。`base` は依存導入まで、
`dev` は開発用 stage、`build` は通常 build、`win` は Windows artifact stage の切り分けに
使う。各 stage は同じ Docker/BuildKit cache lock を共有するため、同じ host で診断 build を
並列実行しない。

context export は固定 commit と clean/pinned submodule の recursive export が全て成功した
後にだけ `.cemu-oracle-context` completion marker を作成する。途中の context は completed
として扱わず、export の root-relative plain log（submodule path、bundle、生成 Dockerfile の
`status=ok|failed`）を保持する。
export 完了時には context の file 数と `du -sh` 要約も表示する。context には `.git`、未追跡
ファイル、credential/config は含まれない。例外として vcpkg manifest の固定
`builtin-baseline` を解決するため、helper は Oracle commit の gitlink から vcpkg の pinned commit
を読み取り、worktree HEAD と一致すること、baseline がその ancestor であることを検証する。
baseline だけを bundle 化する方法は `dev` bootstrap には足りても、full build の version DB が参照する
SDL3 port tree（`read-tree ca8f0f2f…`）を持たず失敗した。このため pinned commit の履歴/object closure
全体を bundle 化する。SDL3 3.4.10 port-version 0 の tree ID は固定 pinned commit の
`versions/s-/sdl3.json` から導出し、source と Docker 内の両方で tree object として検証する。

Oracle 側の ref は変更せず、context 外の一時 bare repository に pinned commit を
`refs/tags/cemu-vcpkg-pinned` として local fetch し、その名前付き ref から bundle を作る。bundle は
この pinned ref だけを広告すること、Git bundle として検証できること、非ゼロかつ 512 MiB 以下である
ことを確認してから context に入る Git object bundle であり、host の `.git` file、Git config、credential
helper、未追跡ファイルを含めない。context 専用の `.cemu-oracle.Dockerfile` はその bundle だけを
root-relative path で vcpkg の新規ローカル Git DB に fetch し、pinned commit、baseline commit、SDL3
tree を確認してから bootstrap/versioning を実行する。固定 Oracle の `Dockerfile` 自体は変更しない。

Docker build の上限時間は `CEMU_CPP_ORACLE_BUILD_TIMEOUT_MINUTES`（既定 30 分）で設定する。
値は非負の整数のみ受け付け、`0` は上限なし（無効）を明示的に意味する。上限到達時は
TERM を送り、10 秒後に残存プロセスを kill して exit 124 とする。失敗または timeout の
場合は context path と secret を含めない build log path を表示し、復旧調査用に両方を保持する。
成功時は一時 context と build log を cleanup する（`CEMU_CPP_ORACLE_KEEP_CONTEXT=1` なら
成功 context も保持）。

## マイルストーン別チェックリスト

### 1. Baseline and oracle — 進行中

- [x] `rust-rewrite-baseline-ab0b7720` を `ab0b7720` に固定した。
- [x] C++ oracle を別の detached worktree に固定した。
- [x] 固定 revision と pinned submodule だけを export する C++ Docker build helper を追加した。
- [x] C++ Docker `base` / `dev` と vcpkg bootstrap の成功証拠をDocker検証ログへ記録した。
- [x] C++ 877 targetsのcompile/linkとCTest 37/37、0 failedを固定`ab0b7720`で確認した。
- [ ] post-test runtime bundlingを完了し、C++ `build` target imageを生成する。
- [ ] guest cycle を時刻とする canonical JSONL trace schema を確定する。
- [ ] 同一入力の反復結果が byte-for-byte で一致することを Docker 内で検証する。
- [x] 412-byte positive fixtureのcanonical 3-record parse/map traceをC++ adapterとRustで
  生成し、Docker内comparator exit 0でexact equalityを確認した。
- [x] `compat/golden` manifestに完全なoracle commitとadapter source hashをpinし、aggregate
  `ci`でRust traceを再生成してgoldenとの比較を毎回実行するようにした。
- [ ] malformed corpusとCafe/MMU/CPU executionを含むC++/Rust parityへ拡張する。
- [x] `cex-trace-compare` exit 0/1/2、match/mismatch/error/no-clobberのRust自己整合性を
  Docker `rust-oracle-smoke`で検証した。
- [x] oracle helperの`bash -n`をDocker内で確認し、trace artifactの`.dockerignore`除外は
  patternを静的確認した。Docker build成功だけをartifact不在の証明には用いない。
- [x] positive synthetic main RPXのvalidation/mapping contractを固定C++ oracleと比較した。
- [ ] fixture manifest が許可済み metadata、digest、期待結果だけを含むことを検査する。

### 2. Headless interpreter — 進行中

- [ ] endian 型、4 GiB guest address space、権限検査を完成させる。
- [ ] PPC interpreter の必要命令と例外経路を完成させる。
- [ ] RPX/RPL loader、scheduler、VFS、最小 Cafe/IOSU を接続する。
- [x] license-clean な合成 CEXH fixture の headless 実行を Docker test で通す。
- [x] fixed synthetic main RPX1 + provider RPL1 positive link-state と near REL24 call executionを
  source payload fingerprint `e20c0843…8dd9` の aggregate Docker buildで再検証した。main raw
  `0x48000001`→linked `0x48002001`、`bl → addi r3,42 → blr → stop`、r3=42、LR/PC、cycles-retired 4を固定。
- [ ] license-clean な最小 RPX/Homebrew を決定的に終了させる。
- [ ] retail RPL/multiple dependencies/cycles/other reloc/import data/compression/TLS/CPU execution/C++ parity を実装・検証する。
- [ ] CPU state、memory digest、event trace を C++ oracle と一致させる（現行7-record execution goldenはRust-only）。
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
| 2026-08-30 | trace comparatorのreportを2 MiBで打ち切り、errorからfilesystem pathを除く | untrusted traceによるmemory/log増幅とhost path漏えいを防ぐ。`TraceReader`にも入力boundsを持たせる。 |
| 2026-08-30 | comparator outputはcaller-owned directory内のrandom mode 0600 tempfileから`persist_noclobber`する | dangling symlinkと既存outputを上書きしないpublicationをLinux Dockerで確認する。atomicityはfilesystem/API実装依存で未証明とし、Windows directory fsync adapterはWindows milestone開始前のblockerとする。 |
| 2026-08-30 | C++ oracle contextはpinned Gitlink historyをsingle advertised ref・512 MiB capでbundleする | baseline-only bundleではSDL3 tree `ca8f…`が欠落したため、必要なpinned object historyだけを318 MiB / 17276 filesのcontextへ含める。 |
| 2026-08-30 | 最初のC++/Rust parity gateをpositive fixtureのparse/map contractに限定する | 412-byte fixtureとcanonical 3-record traceを比較する。Cafe/MMU/CPU execution、malformed corpus、retail RPL/import/relocation/compressionの互換性を意味しない。 |
| 2026-08-30 | C++ adapterは固定oracle worktree/context内のtest-only targetとする | read-only validator/mapping policyを呼ぶだけで、Rust releaseやruntimeのdependencyとしてshipしない。 |
| 2026-08-30 | Linux x86_64 を最初の platform とする | interpreter、x86-64 JIT、headless gate を先に安定させる。 |
| 2026-08-30 | 最初の RPX slice を strict・非圧縮・import-free main module に限定する | code-generated 5-section fixture と deterministic headless/CLI integration で loader の最小契約を固定する。retail RPX/RPL、import、relocation、compression、Cafe OS の対応を意味しない。 |
| 2026-08-30 | 4 GiB guest address space の半開区間終端として `end = 2^32` を許可する | `[start, end)` が address space 末尾まで到達する正当な範囲を表現する。legacy external validator の `UINT32_MAX` 終端制約は互換要件として保持しない。 |
| 2026-08-30 | C++ の旧 JIT にある RWX memory 運用を移植しない | Rust JIT は W^X を必須とし、unsafe を code cache と ABI trampoline に局所化する。 |
| 2026-08-30 | `main` の同期は milestone 間だけにする | 検証中の基準変動を避ける。milestone gate 通過後に同期し、次へ進む前に aggregate `ci` を再実行する。 |
| 2026-08-30 | single synthetic RPX1 + provider RPL1 link slice を固定する | function import/export各1、ADDR32 local→import 2 phase、fresh GuestMemory transactional commit、Text RX/Data RW/Loader R、5 pages/0x5000を対象とする。RPL e_entry=0、nonzero FILEINFO adjustments unsupported fail-closed、module nameはlowercase ASCII basename+exact one `.rpl`、最大recordはindexed O(n log n)とする。retail RPL、複数依存/cycle、他reloc/import data、compression/TLS/CPU execution/C++ parityは未実装。 |
| 2026-08-31 | near REL24 call slice を Rust execution と C++ link-state に分離する | `0x48000001→0x48002001` と `bl→addi r3,42→blr→stop` を固定し、C++ oracleはproduction loader/linkの5-record exact比較のみ、Rust 7-record execution goldenはCPU parityを主張しない。far trampoline/REL14/ADDR16/general parityは未実装。 |

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

### 2026-08-30: main RPX の最小 slice

現在の RPX 実装対象は strict・非圧縮・import-free の main module だけである。
fixture は code から生成する5 section 構成で、deterministic headless/CLI integration へ
接続した。最小sliceは当時の source payload fingerprint `1076852d…9b775` の Docker build で
検証済みだが、retail RPX/RPL、import、relocation、compression、Cafe OS の互換性を
示さない。

### 2026-08-30: 4 GiB 半開区間

guest address range は `[start, end)` で表し、4 GiB address space の末尾を表す
`end = 2^32` を意図的に許可する。`end` を32 bit addressそのものとして扱って
`UINT32_MAX` 以下に制限する legacy external validator の挙動は保持しない。個々の guest
address は従来どおり32 bitだが、exclusive end は一段広い型で表現する。

### 2026-08-30: trace comparatorの安全境界

`cex-trace-compare`はmatchをexit 0、意味差分をexit 1、入力・出力errorをexit 2で返す。
`TraceReader`は入力boundsを検査し、reportは最大2 MiB、errorはpath-freeとする。outputは
caller-owned directory内でrandom name・mode 0600のtempfileへ書き、
`persist_noclobber`で確定するため、dangling symlinkと既存outputを上書きしない。
現在のLinux Dockerではno-replacement publicationを確認したが、普遍的なatomicityは
filesystem/API実装依存で未証明である。Windows directory fsync adapterも未実装である。

`rust-oracle-smoke`はmatch/mismatch/error/no-clobberをDocker内で確認するRust自己整合性
gateである。C++ oracleとのtrace parityを確認するgateではない。

### 2026-08-30: C++ oracle Git object completeness

baseline-only Git bundleではpinned SDL3 tree `ca8f…`が欠落してfull buildに失敗した。
pinned Gitlink historyをsingle advertised ref・512 MiB capでbundleする方式に変更し、
318 MiB / 17276 filesのcontextで`dev`を成功させた。full CEF buildは877 targetsのlinkと
CTest 37/37、0 failedまで成功したが、post-test runtime bundling中にDocker targetが
exit 124となったため、build image/runtime bundleは未完成である。

### 2026-08-30: positive RPX parse/map contract parity

Rustの`cex-rpx-fixture`は412-byteのsynthetic main RPXを生成し、
`cex-rpx-contract-trace`はparse/map結果をcanonical 3-record JSONLへ変換する。固定
`ab0b7720`のC++ adapterはread-only `RPLLoader_ValidateExternalImage`とmapping policyを
呼び、Rust comparatorでpositive fixture traceのexact equalityを確認した。

このgateはparse/map contractだけを対象とする。Cafe/MMU/CPU execution、malformed corpus
parity、retail RPL、import、relocation、compressionは未対応である。C++ adapterは別worktree
と一時Docker contextにだけ存在するtest-only targetで、shipping artifactやRust runtimeの
dependencyには含めない。

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
- trace input/report/temp artifactは`.dockerignore`でDocker contextから除外する。
  comparator errorにもhost pathを含めない。
- CI の online test は mock のみを使う。実 Pretendo はユーザーが明示実行する手動試験
  とし、log を redact し、artifact upload を無効にする。

## Docker検証ログ

| 日時（JST） | 対象 | commit/tree | 結果 | 証拠・備考 |
| --- | --- | --- | --- | --- |
| 2026-08-31 | current REL24/link-state gates | parent `984dd5e786546e7803a6ce879af2a1352f166b38` / `e20c0843…8dd9` | 成功 | Rust 292 tests、fmt/check/Clippy、old RPX + new RPL 5-record golden、Rust-only 7-record execution golden、oracle smoke、UI30、audit/notice。Docker buildはmain sessionのみ。 |
| 2026-08-31 | `./docker-build-cpp-oracle.sh rpl-call-trace` | fixed C++ oracle `ab0b7720` | 成功 | production loader/link 5-record exact comparator。C++はlink-stateのみでCPU/MMU parityではない。 |
| 2026-08-31 | `./docker-build-cpp-oracle.sh trace` / `rpl-link-trace` | fixed C++ oracle `ab0b7720` | 成功 | shared CMake hash変更後に再アテスト。 |
| 2026-08-31 | combined artifact / release scratch / headless | parent `984dd5e7` / `e20c0843…8dd9` | 成功 | main772/0600、provider716/0600、link trace5 records、execution trace7 records、release Cemu823576/0755 SHA `990995…ec13`、headless nonroot UID/GID10001。 |
| 2026-08-31 | intermediate failures | parent `984dd5e7` | 失敗（履歴） | Rust Clippy unused import/too_many_lines→needless_borrow、C++ untracked worktree誤記後にbyte-identical移行、0600/record-count/allowlist等のprebuild findings、release cleanup拒否（未実行）を修正。最終exit0。 |
| 2026-08-31 | historical RPX/RPL link-state gates | parent `8dd149160ea64da0b893180bb99cc0d081efbc3a` / `97c2a8cc…4809b` | 成功（履歴） | Rust 249 tests、old RPX + new RPL 5-record golden、oracle smoke、UI、audit/notice。C++ link-state oracle追加前の履歴。 |
| 2026-08-31 | `./docker-build-cpp-oracle.sh rpl-link-trace` | fixed C++ oracle `ab0b7720` | 成功 | production phase0/phase2、Rust comparator exit 0、exact 5 records。 |
| 2026-08-31 | `./docker-build-cpp-oracle.sh trace` | fixed C++ oracle `ab0b7720` | 成功 | CMake target追加後に再実行。 |
| 2026-08-31 | historical release scratch / headless | parent `8dd149160ea64da0b893180bb99cc0d081efbc3a` / `97c2a8cc…4809b` | 成功（履歴） | release export exit 0、Cemu 816688 bytes/0755 SHA `6e89bc…aecb`、headless nonroot UID/GID 10001。 |
| 2026-08-31 | intermediate failures | parent `8dd14916` | 失敗（履歴） | wrong FILEINFO +0x08、constexpr string_view、too_many_lines、C++ brace欠落 exit2、old manifest CMake SHA drift、new golden dockerignore除外。修正後final exit 0。 |
| 2026-08-31 | historical `./docker-build-rust.sh test` | parent `477c0eae` / `daf75747…7faed` | 成功（履歴） | fmt/check/Clippy、Rust workspace 243 tests（cex-system lib 76 + public rpl integration 2を含む）。C++ link-state oracle追加前。 |
| 2026-08-31 | historical `./docker-build-rust.sh ci` | parent `477c0eae` / `daf75747…7faed` | 成功（履歴） | UI source unchanged、cached rust-ui stage、既存30 UI testsを含む aggregate gate。 |
| 2026-08-31 | historical raw `rust-release` / `rust-headless` build | parent `477c0eae` / `daf75747…7faed` | 成功（履歴） | 各Docker target exit 0。 |
| 2026-08-31 | historical release scratch export | parent `477c0eae` / `daf75747…7faed` | 成功（履歴） | `Cemu` 816688/0755/SHA-256 `13a2300859076fb0617bbaf95a1d1c6d9edde9ca53669e085ba89747aaa9e111`、license noticesのbytes/mode/hashを検証。 |
| 2026-08-31 | initial Docker test/clippy | parent `477c0eae` | 失敗 | 85 diagnostics、E0425 stale helper refs、残3 needless_pass_by_value、E0282 BTreeMap type、semicolon lint、provider nonzero entry fixture failures。修正後に再実行。 |
| 2026-08-31 | intentional discovery/transcription checks | parent `477c0eae` | 失敗（履歴） | 意図的zero hash discovery failureと1-byte transcription failureを確認し、再計算・修正した。 |
| 2026-08-30 | `rust-deny` (`cargo-deny` 0.18.4) | 統合作業 tree | 失敗 | RustSec advisory の CVSS 4.0 を parse できなかった。tool version を 0.20.2 へ更新して再試行した。 |
| 2026-08-30 | `rust-deny` (`cargo-deny` 0.20.2) | 統合作業 tree | 失敗 | workspace 内 path dependency の wildcard version を拒否した。root manifest の各内部 dependency に `version = "0.1.0"` を追加して再試行した。 |
| 2026-08-30 | `rust-clippy` | 統合作業 tree | 成功 | workspace/all-targets の Clippy が成功した。 |
| 2026-08-30 | `rust-test` | 統合作業 tree | 成功 | `cargo test --workspace --all-targets --locked` が成功。Rust test は合計 106 件。 |
| 2026-08-30 | `rust-ui` | 統合作業 tree | 成功 | generator drift、Prettier、TypeScript compiler、ESLint、27 tests、Vite build がすべて成功した。 |
| 2026-08-30 | `rust-deny` (`cargo-deny` 0.20.2) | 統合作業 tree | 成功 | advisories、bans、licenses、sources がすべて成功。未遭遇 license allowance の warning は残るが failure ではない。 |
| 2026-08-30 | `rust-release` | 統合作業 tree | 成功 | Docker local output で `result/rust/Cemu` の export に成功した。 |
| 2026-08-30 | `rust-headless` | 統合作業 tree | 成功 | headless runtime image の生成に成功した。 |
| 2026-08-30 | aggregate `ci` | 統合作業 tree | 成功 | Rust format/check/Clippy/test/audit と UI contract/check/test/build を含む aggregate target が成功した。 |
| 2026-08-30 | C++ oracle `build`（初期履歴） | `ab0b7720` | 未完了 | clean な pinned context の生成までは成功したが、Docker/BuildKit が停止状態になりimageを生成できなかった時点の履歴。後続attemptでcompile/CTestは成功した。 |
| 2026-08-30 | C++ oracle `base`（privacy hardening履歴） | `ab0b7720` | 成功 | context-external relative logsを使い、17274 files / 213 MiBのclean contextからbase targetをDocker buildした時点の履歴。後続のpinned Gitlink history方式で再検証した。 |
| 2026-08-30 | C++ oracle `dev`（privacy hardening履歴） | `ab0b7720` | 成功 | privacy-hardened helperでdev targetとvcpkg bootstrapに成功した時点の履歴。後続の318 MiB / 17276 files contextで再検証した。 |
| 2026-08-30 | C++ oracle `build` / CTest（実行前履歴） | `ab0b7720` | 未実行 | `base` / `dev`だけが成功していた時点の履歴。後続attemptでCTest 37/37は成功したがruntime bundle/imageは未完成。 |
| 2026-08-30 | strict main RPX slice（検証前履歴） | RPX 実装中 tree | 未実行 | parser、5-section fixture、headless/CLI integration を実装した時点では未検証だった履歴。後続の `45e696b0…6acb18` 成功証拠により最小sliceはDocker確認済みとなった。 |

### RPX実装前の dirty-tree 証跡（履歴）

当時の source payload fingerprint は
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

以下の wrapper と展開後 command はすべて、この当時の source payload fingerprint を
対象にした履歴である。最新RPX証跡は次節を参照する。

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

Rust 106 tests / UI 27 tests と、上表の Rust 139 tests / UI 30 tests はいずれも
RPX実装前の履歴として残す。後続の`45e696b0…6acb18` / 172 testsと
`b13c435c…7d29d` / 184 testsもそれぞれ次節以降の履歴である。その履歴後の中間証跡は
`8fec55ee…c4a81` / 205 tests、`e40db320…97e78` / 205 testsはいずれも明示的な履歴であり、現行判定には用いない。
C++ oracle はこの時点では clean context 生成後に BuildKit hang していた。その後
`base` / `dev` が成功した。この履歴時点では完全な `build` / CTest は未実行だったが、
さらに後続のattemptでcompile/link 877/877とCTest 37/37が成功した。post-test runtime
bundleとbuild imageだけが未完成であり、最新結果は後段のc535証跡を参照する。

### Region containment前の RPX source payload 証跡（履歴）

RPX milestone の当時の source payload fingerprint は
`sha256:45e696b081d40b5946ebb6f6e491d8f2b5f5163c92455c51ae88bb79306acb18`
である。本台帳を自己参照から除外し、`result/` など Git が ignore する成果物を含めず、
次の既存定義で算出した。

```sh
{
  git diff --binary HEAD -- . ':(exclude)docs/rust-rewrite.md'
  git ls-files --others --exclude-standard -z -- . \
    ':(exclude)docs/rust-rewrite.md' | sort -z | xargs -0 -r sha256sum
} | sha256sum
```

当時の成功証拠に使用した wrapper と、helper が展開した Docker build は次のとおりである。

```sh
./docker-build-rust.sh ci
./docker-build-rust.sh release
./docker-build-rust.sh headless

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

| 日時（JST） | 対象 | fingerprint | 結果 | 証拠・備考 |
| --- | --- | --- | --- | --- |
| 2026-08-30 | aggregate `ci` attempt 1 | RPX final前 tree | 失敗 | Clippy が `size_of` qualification、missing-fields `Debug`、fixture casts、pass-by-value を検出。修正して再試行した。 |
| 2026-08-30 | aggregate `ci` attempt 2 | RPX final前 tree | 失敗 | 未使用の `ELF_HEADER_SIZE` を検出。削除して再試行した。 |
| 2026-08-30 | aggregate `ci` attempt 3 | RPX final前 tree | 失敗 | test-only code に残った5件の cast lint を検出。修正して再試行した。 |
| 2026-08-30 | aggregate `ci` attempt 4 | RPX final前 tree | 失敗 | test 実行まで到達し、ELF `e_type` の読み取り幅誤りを検出。修正して再試行した。 |
| 2026-08-30 | aggregate `ci` final | `45e696b0…6acb18` | 成功 | Rust fmt/check/Clippy、workspace 172 tests、cargo-deny advisories/bans/licenses/sources、RPC drift、Prettier/tsc/ESLint、UI 30 tests、Vite build、release build、license notice gate がすべて成功した。 |
| 2026-08-30 | `rust-release` | `45e696b0…6acb18` | 成功 | `Cemu` 774696 bytes、mode 0755、SHA-256 `3dd0ed7e…`; `LICENSE.txt` 16725 bytes、mode 0644、SHA-256 `1f256e…`; `THIRD_PARTY_LICENSES.txt` 94701 bytes、mode 0644、SHA-256 `68c283…`。 |
| 2026-08-30 | `rust-headless` | `45e696b0…6acb18` | 成功 | license noticesを含むnon-root headless imageの生成に成功した。 |
| 2026-08-30 | C++ oracle `build` / CTest（RPX初回履歴） | `ab0b7720` | 未実行 | `base` / `dev`成功後もfull buildを開始していなかった時点の履歴。後続でCTestは成功したがimageは未完成。 |

この成功は strict・非圧縮・import-free synthetic main RPX の最初のDocker証拠である。
後続のFILEINFO/loader region containment証拠は次節の`b13c435c…7d29d`、golden導入後の
最終証拠はさらに後の`8fec55ee…c4a81`を参照する。Milestone 2 は
Homebrew/Cafe OS/oracle comparison とretail
RPX/RPL/import/relocation/compression が未完成のため、引き続き進行中である。

### Trace comparator実装前の source payload 証跡（履歴）

当時の source payload fingerprint は
`sha256:b13c435cb580b9de8fe993d6bed5c0b0c93acf05f481e661e5fda80cb8c7d29d`
である。本台帳を自己参照から除外し、`result/` など Git が ignore する成果物を含めず、
次の定義で算出した。

```sh
{
  git diff --binary HEAD -- . ':(exclude)docs/rust-rewrite.md'
  git ls-files --others --exclude-standard -z -- . \
    ':(exclude)docs/rust-rewrite.md' | sort -z | xargs -0 -r sha256sum
} | sha256sum
```

当時の成功証拠に使用した wrapper と、helper が展開した Docker build は次のとおりである。

```sh
./docker-build-rust.sh ci
./docker-build-rust.sh release
./docker-build-rust.sh headless

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

| 日時（JST） | 対象 | fingerprint | 結果 | 証拠・備考 |
| --- | --- | --- | --- | --- |
| 2026-08-30 | aggregate `ci`（safety fix後） | final前 tree | 失敗 | 3 testsで不要な`std::io::Cursor` qualificationをClippyが検出。修正して再試行した。 |
| 2026-08-30 | aggregate `ci` final | `b13c435c…7d29d` | 成功 | Rust fmt/check/Clippy、workspace 184 tests、cargo-deny、RPC/UI cached-valid gate、release build、license notice gate がすべて成功した。 |
| 2026-08-30 | `rust-release` | `b13c435c…7d29d` | 成功 | `Cemu` 774632 bytes、mode 0755、SHA-256 `151b639f…`; `LICENSE.txt` 16725 bytes、mode 0644、SHA-256 `1f256e…`; `THIRD_PARTY_LICENSES.txt` 94701 bytes、mode 0644、SHA-256 `68c283…`。 |
| 2026-08-30 | `rust-headless` | `b13c435c…7d29d` | 成功 | license noticesを含むnon-root headless imageの生成に成功した。 |
| 2026-08-30 | privacy-hardened C++ oracle `base` | `ab0b7720` | 成功 | context-external relative logsを使用し、17274 files / 213 MiB のclean contextからDocker buildに成功した。 |
| 2026-08-30 | privacy-hardened C++ oracle `dev` | `ab0b7720` | 成功 | hardened helperでdev targetとvcpkg bootstrapのDocker buildに成功した。 |
| 2026-08-30 | C++ oracle `build` / CTest（containment時履歴） | `ab0b7720` | 未実行 | privacy hardening後もfull buildを開始していなかった時点の履歴。後続でCTest 37/37は成功したがoracle comparisonは未完了。 |

この当時の証拠はFILEINFO text/data/loader containment、public `RpxMappingRegion`、
fallibleかつgeometrically boundedなCLI readerを含むsynthetic main RPX sliceを対象とする。
Milestone 2全体はHomebrew/Cafe OS/oracle comparisonとretail
RPX/RPL/import/relocation/compressionが未完成のため進行中である。この時点のregion
containment reviewはapprovedだった。golden導入後のcomparator/oracle helper判定は次々節を参照する。

### Trace comparator実装後・golden導入前の source payload 証跡（履歴）

この履歴時点の source payload fingerprint は
`sha256:c535f8aed349a34729f4010ec18ae3ce3d185cb89bbb74cbf4f5f58537500640`
である。本台帳を自己参照から除外し、`result/`などGitがignoreする成果物を含めず、
次の既存定義で算出した。

```sh
{
  git diff --binary HEAD -- . ':(exclude)docs/rust-rewrite.md'
  git ls-files --others --exclude-standard -z -- . \
    ':(exclude)docs/rust-rewrite.md' | sort -z | xargs -0 -r sha256sum
} | sha256sum
```

Rust最終証拠のwrapperと、helperが展開したDocker buildは次のとおりである。

```sh
./docker-build-rust.sh format-fix
./docker-build-rust.sh oracle-smoke
./docker-build-rust.sh ci
./docker-build-rust.sh release
./docker-build-rust.sh headless

docker build --file Dockerfile.rust --progress plain --target rust-format-fix \
  --build-arg RUST_VERSION=1.97.1 \
  --build-arg CARGO_DENY_VERSION=0.20.2 \
  --build-arg CARGO_ABOUT_VERSION=0.9.2 \
  --output type=local,dest=<mktemp> .
docker build --file Dockerfile.rust --progress plain --target rust-oracle-smoke \
  --build-arg RUST_VERSION=1.97.1 \
  --build-arg CARGO_DENY_VERSION=0.20.2 \
  --build-arg CARGO_ABOUT_VERSION=0.9.2 \
  --tag cemu-extend-rust:oracle-smoke .
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

`<mktemp>`はhelperが作る実行時一時directoryである。C++ oracleでは
`./docker-build-cpp-oracle.sh dev`と`./docker-build-cpp-oracle.sh build`を使用した。

| 日時（JST） | 対象 | fingerprint / baseline | 結果 | 証拠・備考 |
| --- | --- | --- | --- | --- |
| 2026-08-30 | `rust-oracle-smoke` attempt 1 | final前 tree | 失敗 | Clippyがbool assertionとpass-by-valueを検出。修正して再試行した。 |
| 2026-08-30 | aggregate `ci` attempt 2 | final前 tree | 失敗 | 4件のborrow type mismatch `E0308`を検出。修正して再試行した。 |
| 2026-08-30 | `rust-format-fix` | `c535f8ae…00640` | 成功 | Docker local output経由のformat fixに成功した。 |
| 2026-08-30 | `rust-oracle-smoke` | `c535f8ae…00640` | 成功 | comparator binaryでmatch/mismatch/error/no-clobberとDocker内`bash -n`を確認。trace artifact exclusionは`.dockerignore` patternの静的確認であり、Docker build成功だけを不在証明にはしない。Rust自己整合性でありC++ parityではない。 |
| 2026-08-30 | aggregate `ci` final | `c535f8ae…00640` | 成功 | Rust fmt/check/Clippy、workspace 193 tests、cargo-deny、RPC/UI cached-valid gate（UI 30 tests）、release build、license notice gateが成功した。release binaryによるcomparator smokeも成功した。 |
| 2026-08-30 | `rust-release` | `c535f8ae…00640` | 成功 | `Cemu` 774640 bytes、mode 0755、SHA-256 `9256ccd6353c4aa54acdaba123f3e83e77ee12829041ef3e3c7e804e65901199`; `LICENSE.txt` 16725 bytes、mode 0644、SHA-256 `1f256e…`; `THIRD_PARTY_LICENSES.txt` 94701 bytes、mode 0644、SHA-256 `68c283…`。 |
| 2026-08-30 | `rust-headless` | `c535f8ae…00640` | 成功 | license noticesを含むnon-root headless imageの生成に成功した。 |
| 2026-08-30 | historical `rust-rpx-contract-artifacts` | prior worktree | 成功（履歴） | `cex-rpx-fixture` と `cex-rpx-contract-trace` から 412-byte fixture / canonical 3-record trace / SHA256SUMS / `cex-trace-compare` を Docker build で生成・検証した。Rust 側の契約 trace は固定 synthetic positive fixture に限ったものとして維持している。 |
| 2026-08-30 | privacy-hardened C++ oracle `base` | `ab0b7720` | 成功 | context-external relative logsを使用し、17274 files / 213 MiBのclean contextからbase targetをDocker buildした。 |
| 2026-08-30 | privacy-hardened C++ oracle `dev` | `ab0b7720` | 成功 | hardened helperのdev targetとvcpkg bootstrapをDocker buildした。 |
| 2026-08-30 | baseline-only C++ history bundle `build` | `ab0b7720` | 失敗 | pinned SDL3 tree `ca8f…`がbundleに存在せず、full buildを開始できなかった。 |
| 2026-08-30 | pinned Gitlink history C++ `dev` | `ab0b7720` | 成功 | single advertised ref、512 MiB cap、318 MiB / 17276 filesのcontextでdev targetとbootstrapに成功した。 |
| 2026-08-30 | full CEF `build` attempt 1 | `ab0b7720` | timeout | 30分で868/877 targetsまで到達した。 |
| 2026-08-30 | full CEF `build` resume 1 | `ab0b7720` | timeout | 15分で877/877をlinkし、CTest 9/37まで到達した。 |
| 2026-08-30 | full CEF `build` resume 2 | `ab0b7720` | 一部成功 | CTest 37/37、0 failed。Docker targetはpost-test runtime bundling中にexit 124となり、image/runtime bundleは未完成。 |

C++ oracleはcompile/linkとCTest 37/37の成功を確認済みだが、`build` target imageとruntime
bundleは未完成である。この節のRust comparator Docker smokeは自己整合性だけであり、
この時点ではC++/Rust trace parity gateは未完了だった。これは履歴であり、現行判定には
次節の最終証跡を用いる。

### Golden導入後・release-staging P2前の source payload 証跡（中間履歴）

当時の source payload fingerprint は
`sha256:8fec55ee14827ba7c85a6830b7989602b5f569c88f9875b913fa4a1069dc4a81`
である。本台帳 `docs/rust-rewrite.md` を自己参照から除外し、`result/`などGitがignoreする
build artifactを入力に含めない。以前の`23f4bd6e…9226e`、`45e696b0…6acb18`、
`b13c435c…7d29d`、`c535f8ae…00640`とこの`8fec55ee…c4a81`はすべて履歴上の
fingerprint/result であり、latestではない。

```sh
{
  git diff --binary HEAD -- . ':(exclude)docs/rust-rewrite.md'
  git ls-files --others --exclude-standard -z -- . \
    ':(exclude)docs/rust-rewrite.md' | sort -z | xargs -0 -r sha256sum
} | sha256sum
```

正式証拠となる以下の実行はすべて main session だけが行った。`./docker-build-rust.sh ci`は
Rust workspace 205 tests、fmt/check/Clippy、cargo-deny、UI 30 tests、license notice、
rust oracle smoke、tracked fixed-C++ golden gateを含む。`rpx-contract-artifacts`は配布用
contract artifactを再生成する。

```sh
./docker-build-rust.sh format-fix
./docker-build-rust.sh ci
CEMU_CPP_ORACLE_BUILD_TIMEOUT_MINUTES=30 ./docker-build-cpp-oracle.sh trace
./docker-build-rust.sh release
./docker-build-rust.sh headless
./docker-build-rust.sh rpx-contract-artifacts
```

| 日時（JST） | 対象 | fingerprint / baseline | 結果 | 証拠・備考 |
| --- | --- | --- | --- | --- |
| 2026-08-30 | `rust-format-fix` | `8fec55ee…c4a81` | 成功 | main sessionでDocker-only format fixを完了。 |
| 2026-08-30 | aggregate `ci` | `8fec55ee…c4a81` | 成功 | Rust workspace 205 tests、fmt/check/Clippy、cargo-deny、UI 30 tests、license notice、rust oracle smoke、tracked fixed-C++ golden gateがすべて成功。CIは毎回Rust traceを再生成し、tracked golden traceと比較する。 |
| 2026-08-30 | C++ oracle `trace` | fixed `ab0b772029f0a5cd57c194cf338003fe8eae8ab8` | 成功 | `CEMU_CPP_ORACLE_BUILD_TIMEOUT_MINUTES=30`。412-byte positive fixtureのcanonical 3-record parse/map traceがRustとexact equality。adapterはtest-onlyである。 |
| 2026-08-30 | `rust-release` | `8fec55ee…c4a81` | 成功 | release exportを生成し、下記artifact manifestを検証。 |
| 2026-08-30 | `rust-headless` | `8fec55ee…c4a81` | 成功 | non-root headless imageとlicense noticesを検証。 |
| 2026-08-30 | `rust-rpx-contract-artifacts` | `8fec55ee…c4a81` | 成功 | fixture、Rust trace、comparator、SHA256SUMSを生成・検証。 |

この時点のartifactは `result/*pre-golden-20260830` として既存出力を保存したまま検証した。
`SHA256SUMS`にはcomparator自身も含まれる。

| artifact | bytes / mode | SHA-256 |
| --- | --- | --- |
| `Cemu` | 774696 / 0755 | `e3a2d5b6827b9355821d74faea9673c4714c3de196946fd4014e0612ac0abac0` |
| `LICENSE.txt` | 16725 / 0644 | `1f256e…` |
| `THIRD_PARTY_LICENSES.txt` | 94701 / 0644 | `68c283…` |
| positive fixture | 412 / 0644 | `ad3657…` |
| Rust trace | 1194 / 0600 | `7a1059…` |
| `cex-trace-compare` | 1036888 / 0755 | `605853…` |
| `SHA256SUMS` | 245 / 0644 | `1d576b…` |
| golden trace | 1194 / 0600 | `7a1059…`（Rust traceとexact同一） |
| `compat/golden` manifest | — | `c8e5bb…` |

このC++/Rust比較はpositive fixtureのparse/map contractだけを対象とする。Cafe/MMU/CPU
execution、malformed corpus、retail RPX/RPL、import、relocation、compressionのparityは
証明していない。`compat/golden` manifestは固定oracleの完全commitとadapter source hashを
pinする。adapter、helper、またはoracle policyを変える場合は、goldenを更新する前に
main sessionで手動C++再認証を行う。

process履歴も保持する。この時点の最終前の重複C++ solveはsource変更のため477/477 link後に停止した。
またreview agentがmain-session-onlyのbuild管理に反して重複実行したため停止し、その結果を
正式証拠から除外した。最終buildはmain sessionだけが実行した。

comparatorのno-clobberはLinux Dockerで確認した範囲に限る。すべてのfilesystem/APIでの
universal atomicityは主張しない。`.dockerignore`は静的なcontext除外policyであり、任意の
Docker context内容が常に不在であることの証明ではない。Windows directory fsync adapterは
Windows milestone開始前のblockerのままである。

### release-staging P2後の最終 source payload 証跡（旧履歴）

当時の source payload fingerprint は
`sha256:e40db320e18f27ca3ad8d0dfe02c0c56a47d25bb4ff51cde87ea1e8da6c97e78`
である。本台帳を除外する同じ算出定義を用いる。直前の`8fec55ee…c4a81`を含む全ての旧
fingerprint/resultは履歴であり、現行判定には用いない。

release-staging P2修正後、main sessionが次をすべてexit 0で実行した。C++ trace attestationは
main sessionの既存exit 0結果を維持する。P2修正は`docker-build-rust.sh`と`.dockerignore`だけで、
C++ adapter/helper/golden/manifest入力を変更していないためである。

```sh
./docker-build-rust.sh ci
./docker-build-rust.sh release
./docker-build-rust.sh headless
./docker-build-rust.sh rpx-contract-artifacts
```

| 日時（JST） | 対象 | fingerprint / baseline | 結果 | 証拠・備考 |
| --- | --- | --- | --- | --- |
| 2026-08-30 | aggregate `ci` | `e40db320…97e78` | 成功 | main sessionでexit 0。Rust traceを再生成し、tracked fixed-C++ golden gateを含めて比較した。 |
| 2026-08-30 | `rust-release` | `e40db320…97e78` | 成功 | main sessionでexit 0。release-staging P2 hardening後のexportを検証。 |
| 2026-08-30 | `rust-headless` | `e40db320…97e78` | 成功 | main sessionでexit 0。 |
| 2026-08-30 | `rust-rpx-contract-artifacts` | `e40db320…97e78` | 成功 | main sessionでexit 0。fixture、Rust trace、comparator、SHA256SUMSを再生成・検証。 |
| 2026-08-30 | C++ oracle `trace` | fixed `ab0b772029f0a5cd57c194cf338003fe8eae8ab8` | 有効 | P2後もC++入力は不変。main sessionでexit 0を確認済みのattestationを維持する。 |

P2修正はrelease staging directory `.docker-rust-release.*/` をDocker contextから除外し、
markerを検証してcleanupし、wrapperの終了statusを明示するものである。最終export後に
`.docker-rust-release.*` と `.docker-rpx-contract.*` directoryが残っていないことを確認した。
artifactのbytes/mode/hashは直前の中間履歴と不変である。`SHA256SUMS`にはcomparator自身が
含まれる。

| artifact | bytes / mode | SHA-256 |
| --- | --- | --- |
| `Cemu` | 774696 / 0755 | `e3a2d5…` |
| `LICENSE.txt` | 16725 / 0644 | `1f256e…` |
| `THIRD_PARTY_LICENSES.txt` | 94701 / 0644 | `68c283…` |
| positive fixture | 412 / 0644 | `ad3657…` |
| Rust trace | 1194 / 0600 | `7a1059…` |
| `cex-trace-compare` | 1036888 / 0755 | `605853…` |
| `SHA256SUMS` | 245 / 0644 | `1d576b…` |
| golden trace | 1194 / 0600 | `7a1059…`（Rust traceとexact同一） |
| `compat/golden` manifest | — | `c8e5bb…` |

この最終証跡もpositive fixtureのparse/map contractに限定する。Cafe/MMU/CPU execution、
malformed corpus、retail RPX/RPL、import、relocation、compressionのparityは未証明である。
universal atomicityは主張せず、`.dockerignore`は静的policyであって全Docker context内容の
不在証明ではない。Windows directory fsync adapterはWindows milestone開始前のblockerである。

### RPX1/RPL1 link slice の旧 source payload 証跡

当時の source payload fingerprint は
`sha256:daf75747d62620c63e805a6af07fa0b12f3576eda450992ec08446d7e4a7faed`
である。本台帳自身を除外し、本slice差分の親commitは
`477c0eaea0b33ed80f91e7acf4827a54812663c9`である。固定C++互換基準は引き続き
`ab0b772029f0a5cd57c194cf338003fe8eae8ab8`とする。
今回のsingle synthetic pairは、main RPX1 + provider RPL1、function import/export各1、
ADDR32 local→import 2 phase、fresh GuestMemory transactional commit、Text RX/Data RW/Loader R、
5 pages/0x5000、local site `0x10002008`、import site `0x10000000`、value `0x02002000`を含む。
RPL `e_entry=0`、nonzero FILEINFO adjustments unsupported fail-closed、module nameはlowercase ASCII
basename + exact one `.rpl`、最大recordはindexed O(n log n)である。

正式main-session Docker evidence:

```text
./docker-build-rust.sh test                         exit 0
./docker-build-rust.sh ci                           exit 0
docker build --target rust-release ...              exit 0
docker build --target rust-headless ...             exit 0
docker build --target rust-release --output ...     exit 0
```

`test`はfmt/check/ClippyとRust workspace 243 tests（cex-system lib 76 + public rpl integration 2を含む）を通過。
`ci`はUI source unchanged、cached rust-ui stage、既存30 UI testsを検証した。release artifactは
`Cemu` 816688 bytes/mode0755/SHA-256 `13a2300859076fb0617bbaf95a1d1c6d9edde9ca53669e085ba89747aaa9e111`、
`LICENSE.txt` 16725/0644/SHA-256 `1f256ecad192880510e84ad60474eab7589218784b9a50bc7ceee34c2b91f1d5`、
`THIRD_PARTY_LICENSES.txt` 94701/0644/SHA-256 `68c283806c8fc31007234f594ba8dea3a970a791bb9ba98017239900eb9a9e51`である。

失敗履歴は保持する。初回Docker test/clippyの85 diagnostics、E0425 stale helper refs、残3件の
needless_pass_by_value、意図的zero hash discovery failureと1-byte transcription failure、review P2 4件
（二次走査/RPL entry/one-past symbol/nonzero adjustment）対応、E0282 BTreeMap type、semicolon lint、
provider nonzero entry fixture failuresを経て最終exit 0に到達した。architecture Sol-high + safety
Sol-medium final read-only reviewはP0/P1/P2なしで承認し、P3はlegacy cfg(test) validator driftとbounded
BTree node OOM変換不可として残す。既存e40db/205、daf757/243、97c2/249は明示的履歴へ降格し、現行/最新表現はe20c/292に一本化する。
C++ positive parse/map attestationは前sliceのまま有効だが、新RPL linkのC++ parity証拠ではない。

検証時は実行した完全な `docker build` command、対象 commit または dirty tree の識別、
成否、失敗した target/test、必要な log の保存先をこの表へ追記する。秘密情報を含む log
は保存せず、redact した要約だけを記録する。

### RPX/RPL link-state の旧 source payload 証跡

当時の source payload fingerprint は `sha256:97c2a8cc03d6cc6a87adc813e42bd46fbb72814393da98fb7d10ab5fd3b4809b`、parent HEAD は `8dd149160ea64da0b893180bb99cc0d081efbc3a` である。これはC++ call-state oracle追加前の履歴である。
この検証済みpayloadはcheckpoint commit `0d240acd` (`Add C++ RPL link-state oracle`)として保存した。
固定 synthetic main RPX1 + provider RPL1 positive link-stateのみを対象とし、provider `e_entry=0`のためtest-only adapterがexternal-linkage production `ProcessHeaders/LoadSections/UpdateEntrypoint(mainのみ)/LinkSingleModule`を使用する。phase loopとpermission/hash projectionはharness-definedであり、public loader/full lifecycle、CPU execution、retail/general relocation、MMU permission parityではない。

正式main-session Docker evidenceは `./docker-build-rust.sh rpl-link-contract-artifacts`、`./docker-build-cpp-oracle.sh rpl-link-trace`、`./docker-build-cpp-oracle.sh trace`、`./docker-build-rust.sh ci`、release scratch export、`./docker-build-rust.sh headless` がすべてexit 0。Rust 249 tests、old RPX golden + new RPL 5-record golden、oracle smoke、UI、audit/notice、C++ comparator exact 5 recordsを確認した。

artifact hashes: main 772/0644 `13011586…374b`、provider 716/0644 `95295415…8cec`、trace 1796/0600 `e5da8b…9907e`、comparator 1036888/0755 `605853…254f`、SHA256SUMS 321/0644 `7c9e75…7aee`、manifest `059cb0…4fc2`、CMake `bd87…06b7`、adapter `07af…70e9`、memory hash `6f0b03…46ae`。release Cemu 816688/0755 `6e89bc…aecb`、LICENSE 16725/0644 `1f256e…`、notices 94701/0644 `68c283…`、headless nonroot UID/GID 10001。

失敗履歴は保持する（wrong FILEINFO +0x08、C++ constexpr string_view、Clippy too_many_lines、C++ memory/terminal root brace欠落、old RPX manifest CMake SHA drift、new golden dockerignore除外）。修正後final exit 0。no-clobber atomicityはfilesystem依存、dockerignoreは静的policy、Windows dir fsync blockerは継続。

### near REL24 call execution の現行 source payload 証跡

現行 source payload fingerprint は `sha256:e20c0843060bde8656512a1119ea0efe1ff4041a77c14384456230d5e5498dd9`、parent HEAD は `984dd5e786546e7803a6ce879af2a1352f166b38`（`984dd5e7 Execute linked REL24 calls headlessly`）である。本台帳自身と`result/`を除外した既存式で architecture+safety が独立再計算した。
固定 synthetic main RPX1 + provider RPL1 のpositive link-stateとRust-only near `R_PPC_REL24` call executionだけを対象とする。main raw instruction `0x48000001`はlinked `0x48002001`、provider ADDR32 site `0x10002008`は`0x02002000`、main REL24 site `0x02000000`のdisplacementは8192。headlessは`bl → addi r3,42 → blr → stop`、final r3=42、LR `0x02000004`、PC `0x02000008`、cycles/retired 4、linked 5 pages/20480、executed 21 pages/86016を固定した。far trampoline/REL14/ADDR16/general parityは未実装である。
provider `e_entry=0`のためtest-only C++ adapterがexternal-linkage production `ProcessHeaders/LoadSections/UpdateEntrypoint(mainのみ)/LinkSingleModule`を使う。C++ oracleはlink-stateのみを証明し、CPU実行とMMU permission parityは証明しない。phase loopとpermission/hash projectionはharness-definedであり、Rust 7-record execution goldenはRust-onlyである。

正式main-session Docker evidenceは `./docker-build-rust.sh rpl-link-contract-artifacts`、`./docker-build-cpp-oracle.sh rpl-call-trace`、`./docker-build-cpp-oracle.sh rpl-link-trace`、`./docker-build-cpp-oracle.sh trace`、`./docker-build-rust.sh ci`、release scratch export、`./docker-build-rust.sh headless` がすべてexit 0。Rust 292 tests、old/new golden、audit/notice、UI30 tests/build、C++ production 5-record exact comparatorを確認した。
artifactはmain 772/mode600 SHA `e5e0069938c0a575a9d14c1ab4a79f8c91c4c58e85c10a1251a4a9522d204223`、provider 716/mode600 SHA `9529541572c1eb5d83b2cf89f02f6b6daace501a9a8ac15e2f715a5229b68cec`、link trace 2195/mode600/5 records SHA `e39f587781607351e6d468acbf39f9eac1c2281d57d6c849cf4337d30ce2d330`、execution trace 4778/mode600/7 records SHA `1ca5a6b1c93485366b556bda289569f6157b949717d2523ede9f09a5150dedf7`、comparator 1036888/0755 SHA `605853…254f`、SHA256SUMS 419/0644 SHA `1846b6…`。CMake SHA `4315c68f…fac3`、adapter SHA `0060304a…fb3d6`、memory hash `6f0b03…46ae`。release Cemu 823576/0755 SHA `990995aa5a00ed068939d2a57534a555e2395e58b88e4aee2bb3f954fdeecc13`、LICENSE 16725/0644 SHA `1f256e…`、notices 94701/0644 SHA `68c283…`、headless nonroot UID/GID 10001。

失敗履歴は保持する。初回Rust trace DockerのClippy unused import/too_many_lines、次回2 needless_borrow、C++ fixed worktreeへのuntracked誤記（byte-identical移行・削除でclean復元）、0600/record count/allowlist等のprebuild findings、release recursive cleanup拒否（未実行）を記録し、修正後final exit 0とした。

## レビュー指摘と対応

| 日時（JST） | レビュー | 指摘 | 対応 | 状態 |
| --- | --- | --- | --- | --- |
| 2026-08-31 | architecture Sol-high + safety Sol-medium code/evidence review | near REL24 call execution、link-state、transactional memory、RPL metadata、import/reloc安全性を確認。 | 現行`e20c0843…8dd9` / 292 testsの実装と証拠を確認し、fingerprint exact、P0/P1/P2/P3なし。C++はlink-stateのみ、Rust executionはCPU parityを主張しない。 | 最終承認済み |
| 2026-08-31 | final golden/Dockerignore diff review | 現行5-record link golden、7-record execution golden、manifest、CMake/adapter hash、Dockerignore差分。 | architecture / safetyがgolden SHA・record数・manifest pin・literal re-include・4本のaggregate gateを再確認し、fingerprint `e20c0843…8dd9`を独立再計算して一致。P0/P1/P2/P3なし。 | 最終承認済み |
| 2026-08-31 | architecture Sol-high + safety Sol-medium code review（旧） | RPX1/RPL1 link sliceの境界、transactional memory、RPL metadata、import/reloc安全性を確認。 | `97c2a8cc…4809b` / 249 tests時点の実装と証拠を確認。 | 履歴 |
| 2026-08-31 | final golden/Dockerignore review（旧） | 新RPL 5-record golden、manifest、CMake/adapter hash、Dockerignore差分。 | `97c2a8cc…4809b`時点の確認。 | 履歴 |
| 2026-08-30 | architecture / safety final read-only review | fingerprint `e40db320…97e78` の正式Docker証拠、golden pin、P2 hardening後のC++ trace attestationを対象にdoc+codeを読む最終確認。 | architecture・safetyの両reviewerがfingerprintを再計算し、code・script・台帳を確認した。未解決P0/P1/P2なし。 | 最終承認済み |
| 2026-08-30 | architecture review | `rust-ci` に dependency/advisory/license audit がなく「完全な gate」と呼べない。 | `rust-deny` を `rust-ci` の祖先へ追加し、aggregate `ci` を正式 gate に変更。0.18.4 の CVSS 4.0 parse failure 後、0.20.2 へ固定して全 audit を通した。 | 対応済み・Docker 確認済み |
| 2026-08-30 | architecture review | RPC manifest 変更時に Rust/TS generated contract drift を検知する必要がある。 | Rust 側 golden test を `rust-test` に追加し、generator drift と UI checks を行う `rust-ui` を aggregate `ci` へ接続。 | 対応済み・Docker 確認済み |
| 2026-08-30 | architecture review | oracle/comparator 用 Docker target と証拠場所がまだない。 | C++ compile/linkとCTest 37/37は成功。post-test runtime bundlingのexit 124によりbuild imageは未完成。Rust `rust-oracle-smoke`は自己整合性のみで、C++ parityは未完了gateとして維持。 | 一部対応・parity/image未完了 |
| 2026-08-30 | architecture review | RPX slice の対応範囲が完成実装と誤認されないこと。 | 当時のfingerprint `c535f8ae…00640`、workspace 193 testsのDocker buildでstrict・非圧縮・import-free synthetic main moduleを再検証し、未実装範囲を維持した。 | 履歴 |
| 2026-08-30 | architecture review | 4 GiB末尾までの半開区間を legacy validator が拒否する。 | exclusive endに`2^32`を許可し、workspace 193 testsを含むaggregate Docker buildで再検証した。 | 履歴・現行承認は上段参照 |
| 2026-08-30 | RPX final review | parser、fixture、CLI/headless integration、4 GiB境界の最終レビューが必要。 | architecture / safety reviewerが当時のDocker証跡と未実装範囲を確認した。 | 履歴・現行承認は上段参照 |
| 2026-08-30 | architecture final review | FILEINFO text/data/loader regionのcontainmentが曖昧。 | 各regionを明示的にcontainし、境界・重複・範囲外を当時の193 testsとaggregate Docker buildで再検証した。 | 履歴・現行承認は上段参照 |
| 2026-08-30 | architecture final review | mapping結果を内部tupleへ閉じ込めると境界契約が不明瞭。 | public `RpxMappingRegion` を導入してmapping regionを型で公開した。 | 履歴・現行承認は上段参照 |
| 2026-08-30 | safety final review | CLI readerがpanicまたは無制限allocationへ進み得る。 | fallibleかつgeometrically boundedなreaderへ変更し、malformed input経路をDocker testで検証した。 | 履歴・現行承認は上段参照 |
| 2026-08-30 | privacy final review | oracle helper logがexport context内へ入り得る。 | logをcontext-externalなrelative locationへ移し、privacy-hardened `base` / `dev`を固定`ab0b7720`で再実行した。 | 履歴・現行承認は上段参照 |
| 2026-08-30 | architecture final review | comparatorのexit contractとresource boundsが曖昧。 | exit 0/1/2、bounded `TraceReader`、2 MiB report capを公開CLI契約として固定し、release binary smokeで検証した。 | 履歴・現行承認は上段参照 |
| 2026-08-30 | safety final review | output race、symlink、既存file上書き、path漏えいの危険。 | caller-owned directory内のrandom mode 0600 tempfile、`persist_noclobber`、dangling symlink/existing output rejection、path-free errorsをLinux Docker testで確認した。 | 履歴・現行承認は上段参照 |
| 2026-08-30 | privacy final review | trace artifactとoracle helper metadataがDocker contextへ混入し得る。 | `.dockerignore`除外はpatternを静的確認し、Docker内`bash -n`、single-ref Gitlink history bundle、context-external relative logsはsmoke/base/devで確認した。Docker build成功だけをtrace artifact不在の証明には用いない。 | 履歴・現行承認は上段参照 |
| 2026-08-30 | portability review | directory fsyncのWindows adapterがない。 | Linux scopeを明記し、Windows milestone開始前のblockerとして次作業へ登録した。 | Windows着手前blocker |
| 2026-08-30 | architecture / safety code review | trace comparatorとoracle helper hardeningの最終確認。 | 当時の`c535f8ae…00640` / 193 testsの証拠へ台帳を更新し、C++ CTest成功とimage未完成を分離した。 | 履歴 |
| 2026-08-30 | architecture review | CI path filter が generator と generated files を含んでいない。 | contract、generator、generated Rust/TS、本台帳に加え、`.dockerignore`、`compat/**`、`ui/**`、root `LICENSE.txt`、`licenses/**` を trigger 対象へ追加。凍結した C++ oracle header は生成対象に含めない。 | 対応済み・静的確認済み |
| 2026-08-30 | architecture review | action/base image の可変 tag に対して再現性の表現が広すぎる。 | 再現性の主張を locked Cargo graph と同一入力の再実行性に限定。digest pin は未対応事項として残す。 | 文書対応済み |
| 2026-08-30 | safety review | 初期実装の指摘は未回収。 | review を回収し、以下の個別指摘へ展開した。 | 回収済み |
| 2026-08-30 | safety review | headless trace output preservation と code-size validation が未完了。 | headless trace出力保持とcode-size検査を追加し、当時のfingerprint `c535f8ae…00640`のworkspace 193 testsとheadless image buildを通した。 | 履歴 |
| 2026-08-30 | safety review | Docker secret exclusions が未確認。 | `.dockerignore`へtrace artifactを追加し、当時のfingerprint `c535f8ae…00640`のaggregate `ci`、release、non-root headlessを通した。 | 履歴 |
| 2026-08-30 | safety review | trace semantics と integration compile drift の検証が未完了。 | bounded `TraceReader`とcomparator smokeを追加し、当時のfingerprint `c535f8ae…00640`のworkspace 193 testsとaggregate `ci`を通した。 | 履歴 |
| 2026-08-30 | final integration review | Clippy が VFS documentation、GPU、CEMOD、migrate test の lint error を検出した。 | 各errorを修正し、当時のfingerprint `c535f8ae…00640`のClippyとaggregate `ci`を通した。全失敗行はDocker検証ログに保持した。 | 履歴 |
| 2026-08-30 | final integration review | `cargo-about` 0.9.2 を default feature で install すると CLI command が存在しない。 | install に `--features cli` を追加し、release notices と aggregate notice gate を通した。 | 対応済み・Docker 確認済み |
| 2026-08-30 | safety review | release/headless に license bundle と non-root runtime の最終確認が必要。 | 当時のfingerprint `c535f8ae…00640`のaggregate notice gate、release license bundle、non-root headlessをDocker buildで確認した。 | 履歴 |
| 2026-08-30 | process review | 2 subagent が Docker-build-only 制約に反して `docker run` test を実行した。 | 当該結果を正式証拠から除外し、同じ対象を最終 aggregate `ci` の Docker build で再検証した。 | 証拠是正済み |
| 2026-08-30 | safety review | `rust-builder`、`rust-headless`、`rust-release` が `Cemu` binary だけを出力し、root license と Rust dependency notice を同梱していない。 | `cargo-about` 0.9.2 を Docker 内 CLI tool として固定し、root `LICENSE.txt` と `THIRD_PARTY_LICENSES.txt` を `rust-builder` で生成・検査する。aggregate `ci` は生成済み bundle を copy して notice gate を必須化する。notice は filesystem path を出力せず、path/credential-like field 混入も Docker gate で拒否する。`rust-headless` は `/usr/share/licenses/cemuextend/`、`rust-release` は `result/rust/` へ同梱する。 | 対応済み・Docker 確認済み |

レビューで gate を満たさないと判明した項目は、完了 checkbox を未完了へ戻す。対応を
実装しただけでは解決済みにせず、Docker-only 再検証の行をリンクして閉じる。

## 次の作業

1. [完了] 現行RPX1/RPL1 + near REL24 evidence sliceを`984dd5e7`親のdirty payloadとして記録した。
2. [完了] near `R_PPC_REL24` の parse/link、production C++ link-state 5-record exact比較、Rust-only
   7-record headless executionをDocker buildの292 testsで確認した。`bl -> addi r3,42 -> blr -> stop`、
   `r3=42`、LR/PC、cycles-retired 4、memory proofを固定済み。C++ CPU/MMU parityは未証明。
3. [進行中] ADDR16_HA+LO imported dataを次sliceとして実装し、現行のRust/C++ link-stateとRust-only
   execution gateを維持する。その後、malformed corpus、compressionへ段階的に広げる。
4. adapter、helper、oracle policyを変更する場合は、manifest pinを更新する前にmain sessionで
   手動C++再認証を行う。Docker-only aggregate `ci`、release、headless、contract artifact
   gateの成功を維持する。
5. post-test runtime bundlingを再開して固定`ab0b7720`のC++ `build` target imageを完成させる。
6. Windows milestone開始前にdirectory fsync adapterを実装し、Linux限定で確認した
   no-replacement/durability前提をportableな契約へ拡張する。filesystem依存のatomicityは
   別途検証する。
