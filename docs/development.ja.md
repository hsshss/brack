# Brack 開発メモ

[English](development.md) | 日本語

Brack を作る側のための文書です。構成、ビルド、テストと道具、リリース、文書とコメントの書き方、プラットフォームごとの状態を説明します。使い方は [README](../README.ja.md)、利用者向けの技術的な説明は [details.ja.md](details.ja.md)、設計の判断とその理由、調査の結果は [design-notes.md](design-notes.md) にあります。

## 構成

```
include/brack/brack.h       公開 C API
src/core/                   エンジン本体（静的ライブラリ）
  engine.*                  ホストスレッド、ルーティンググラフ、レンダリング
  plugin/                   形式共通のプラグインインターフェース、スキャン、エディタ用ウィンドウ、
                            OS ごとのプラグインファイルの規則（plugin_files.*）
  clap/  vst3/  vst2/       各形式のモジュール読み込み、インスタンス、エディタ
  audio/                    出力（miniaudio）、SRC（r8brain）
  midi/                     MIDI 入力（WinMM）、仮想ポート（MidiSrv）、Linux は ALSA シーケンサー（alsa/）、
                            macOS は CoreMIDI（coremidi/）
  remote/                   プラグインホストプロセスとの通信（両側）
  session.cpp               セッション JSON
src/host/                   プラグインホストプロセス（brack-host-<アーキテクチャ>）
src/dll/  src/cli/  src/gui/
src/gui/icon/               アプリのアイコン（SVG と、そこから書き出した各 OS 用のファイル）
bindings/dotnet/            .NET バインディング（Brack）とそのテスト
tests/                      テスト用シンセ（CLAP / VST3 / VST2）とテスト
tools/vmidi_probe/          仮想ポートの診断ツール
tools/midi-timing/          MIDI の時刻の計測（設計メモの 15 章）
tools/app-icon/             アイコンの SVG から各 OS 用のファイルを書き出す
tools/linux_syntax_check.sh 共通コードを Linux の g++ で x64・x86・ARM64 向けに構文チェック（WSL などで実行）
tools/arch_matrix.sh        CPU アーキテクチャの組み合わせを、ビルドと動かし方ごとに確かめる（macOS、Linux、Windows）
tools/arm64ec_synth.ps1     テスト用シンセの ARM64EC 版と ARM64X 版を作る（Windows）
tools/check_release_files.sh 配布物と NuGet パッケージのファイルを一覧と照らし合わせる（リリース）
cmake/toolchains/           Linux の x86 版・ARM64 版を x64 の Linux から、x64 版を ARM64 の Linux からビルドするツールチェーン
tools/linux_release_build.sh Linux の配布物を manylinux_2_28 のコンテナでビルドする（リリース）
tools/linux_smoke_test.sh   Linux の配布物をほかのディストリビューションのコンテナで起動する（リリース）
docs/details.md             README の補足（利用者向けの技術的な説明）
docs/design-notes.md        実装の判断と調査結果
```

依存ライブラリは、構成するときに git で取得します: [CLAP](https://github.com/free-audio/clap)、[VST3 pluginterfaces](https://github.com/steinbergmedia/vst3_pluginterfaces)、[vst2sdk](https://github.com/Xaymar/vst2sdk)、[r8brain-free-src](https://github.com/avaneev/r8brain-free-src)、[miniaudio](https://github.com/mackron/miniaudio)、[nlohmann/json](https://github.com/nlohmann/json)、[Dear ImGui](https://github.com/ocornut/imgui)、[GLFW](https://github.com/glfw/glfw)。vst2sdk の定義のうち、実際のプラグインと食い違うものは `src/core/vst2/vst2_abi.h` で置き換えています（設計メモの 6 章）。

`THIRD_PARTY_NOTICES.md` は、配布物に入るライブラリと、ソースに写したほかのプロジェクトの記述を 1 つずつ挙げ、そのライセンスを載せています。`cmake --install` と NuGet パッケージがこれを入れます。依存ライブラリを足したり版を上げたりしたときは、このファイルを、そのライブラリの今のライセンス文に合わせて直します。

利用者に見える名前は「Brack」と書きます。ウィンドウの題名、ダイアログ、ログとエラーの文、CLI の見出し、プラグインに伝えるホスト名（CLAP、VST3、VST2）、Windows MIDI Services のセッションと仮想ポートの説明、仮想ポートの既定の名前、`brack_version_string()`、文書の地の文が該当します。識別子は小文字のままです（ファイルとフォルダの名前、設定フォルダ、`brack-session` / `brack-settings` の形式 ID、C API、CMake のターゲットとオプションなど）。CLI の使い方の表示は、実行ファイルの名前（`brack-cli`）で書きます。

## ビルド

どの OS でも、ビルドとテストは次の形です。成果物は `build/bin/` にできます。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
```

オフラインでビルドするときは、`-DFETCHCONTENT_SOURCE_DIR_CLAP=...` などでローカルのソースを指定します。

| オプション | 既定 | 内容 |
|---|---|---|
| `BRACK_BUILD_GUI` | ON（Linux では、X11 と OpenGL の開発用ファイルがあるとき） | GUI（Dear ImGui + GLFW）。macOS では Brack.app |
| `BRACK_BUILD_CLI` | ON | CLI |
| `BRACK_BUILD_TESTS` | ON | テスト、テスト用プラグイン（CLAP / VST3 / VST2）、診断ツール |
| `BRACK_VERSION` | `1.0.0` | 版（`1.2.0`、プレリリースは `1.2.0-beta.1` の形）。`brack_version_string()` と CLAP のホストが伝えます。Brack.app の `Info.plist` には数字の部分だけが入ります。リリースではタグから決まります（下の「リリース」） |
| `BRACK_OTHER_ARCH_BINS` | `build/bin`、`build-x86/bin`、`build-arm64/bin` のうち自分以外（macOS では `build-x86` の代わりに `build-x64`。Linux では、ARM64 の機械で `build` の隣に置く x64 版のため、`build-x64/bin` も） | ほかのアーキテクチャのビルドの `bin`（`;` 区切り）。同梱するプラグインホストを、そこからこのビルドの `bin` にコピーします |

### 同梱するプラグインホスト

各アーキテクチャの Brack は、動かせるアーキテクチャのプラグインホスト（`brack-host-<アーキテクチャ>`、Windows では `.exe`）を同梱します。どのアーキテクチャかは、[詳しい説明の表](details.ja.md#ほかのアーキテクチャのプラグイン)のとおりです。

- 自分以外のプラグインホストは、そのアーキテクチャのビルドの `bin` からビルドのたびにコピーします。同梱しないものは `bin` から消します。そのため、x64 版と ARM64 版を作るには x86 版を、ARM64 版には x64 版も先にビルドしておきます。足りないものがあると、ビルドが警告します。
- 作り直したときは、先にビルドしたほうをもう一度ビルドします（何もコンパイルせず、コピーだけします）。ビルドの違うプラグインホストは起動を拒み、その旨を報告します（「the plugin host is from another build of Brack」）。
- どの組み合わせで何が動くかは、`tools/arch_matrix.sh` で一度に確かめられます（macOS、ARM64 と x64 の Linux、ARM64 と x64 の Windows。設計メモの 15 章）。
- Apple シリコン版は、Rosetta 2 が入っていれば x64 のプラグインホストを使います。入っていなければ、x64 のプラグインは「this computer does not run x64 programs」で読み込めません。ユニバーサル版を Rosetta 2 で（x64 として）起動しても、ARM64 のプラグインは ARM64 のプラグインホストがそのまま動かします。

### Windows

Visual Studio 2022 以降（C++ によるデスクトップ開発。C++20、CMake と Ninja を含む）が要ります。

x64 版と x86 版は互いのプラグインホストを、ARM64 版は x64 と x86 のものを同梱します。そのため、x86 版、x64 版、x86 版（もう一度、コピーだけ）、ARM64 版の順にビルドします（下の「Windows で」も参照）。

1. 「x86 Native Tools Command Prompt」（または `vcvars32.bat` を実行したコマンドプロンプト）で、x86 版を `build-x86` にビルドします。

   ```bat
   cmake -S . -B build-x86 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
   cmake --build build-x86
   ```

2. 「x64 Native Tools Command Prompt」で、x64 版を `build` にビルドします（上の `build-x86` を `build` にします）。
3. x86 版の環境で、もう一度 `cmake --build build-x86` を実行します（x64 のプラグインホストをコピーし直すだけです）。
4. ARM64 版は `build-arm64` にビルドします。「ARM64 Native Tools Command Prompt」を使うか、x64 の PC では `vcvarsamd64_arm64.bat` を実行したコマンドプロンプトを使います（x64 の上で動く ARM64 用のコンパイラー、Visual Studio の `VC.Tools.ARM64` が要ります）。テストは ARM64 の PC で実行します。

- `build\bin` に `brack.exe`（GUI）、`brack-cli.exe`、`brack.dll` と、同梱するプラグインホストができます。`cmake --install` では、`bin` にこれらが、`include` に `brack.h` が、`lib` に `brack.lib` が入ります。
- ARM64 の PC では、x64 版と x86 版も `vcvarsarm64_amd64.bat` / `vcvarsarm64_x86.bat` で `build` と `build-x86` にビルドできます。テストは Windows のエミュレーションで動きます。
- MSVC では、同じ名前のターゲット（同じフォルダにできる `brack.exe` と `brack.dll`、CLAP と VST3 のテスト用シンセ `brack-test-synth`）が同じ `.pdb` と `.ilk` を書こうとします。そこで、リンカーのファイルを分けたうえで（GUI の PDB は `brack-gui.pdb`、`/INCREMENTAL:NO`）、`add_dependencies` で順にリンクします。リンカーは同じ名前の古い `.ilk` を消すので、同時にリンクすると、もう一方の `.ilk` を消してしまうためです。

### Linux

g++ 13 以上、CMake 3.25 以上、Ninja、git が要ります。Ubuntu 24.04 では次のパッケージを入れます。

```bash
sudo apt install build-essential cmake ninja-build git pkg-config libasound2-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl-dev
```

- ALSA の開発用ファイル（`libasound2-dev`）が無ければ、MIDI ポート無しでビルドします（「MIDI ports are not available on this system yet」）。
- X11 と OpenGL の開発用ファイル（`libx11-dev` から `libgl-dev` まで）が無ければ、GUI 無しでビルドします。X11 の開発用ファイル（`libx11-dev`）が無ければプラグインのエディタも無しでビルドし、エディタを開こうとすると「plugin editors are not available on this system yet」になります。
- C++ の実行時ライブラリはどのプログラムにも静的にリンクします（実行に要るものは[詳しい説明の「配布物の中身」](details.ja.md#配布物の中身)）。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
cmake --install build --prefix ~/.local
```

- `cmake --install` では、`bin` に `brack`（GUI）と `brack-cli`、`lib` に `libbrack.so`、`libexec/brack` にプラグインホスト、`include` に `brack.h` が入ります（プラグインホストを探す場所は[詳しい説明の「配布物の中身」](details.ja.md#配布物の中身)）。
- x86 版と ARM64 版は、x64 の Linux からクロスコンパイラー（Ubuntu の `g++-i686-linux-gnu`、`g++-aarch64-linux-gnu`）でビルドできます。x64 版が x86 のプラグインホストを同梱するように、x86 版を先にビルドします。x86 版と x86 のプラグインホストを x64 の Linux で動かすには、32 ビットの C ライブラリ（Ubuntu の `libc6-i386`）が要ります。
- クロスビルド（x64 の Linux からの x86 版と ARM64 版、ARM64 の Linux からの x64 版と x86 版）は、そのアーキテクチャの ALSA、X11、OpenGL の開発用ファイルが無いので、MIDI ポート、プラグインのエディタ、GUI がありません。ツールチェーンは、ライブラリとヘッダーを `/usr/aarch64-linux-gnu` などの下でしか探さないためです。
  - クロスビルドのプラグインホストを同梱すると、そのアーキテクチャのプラグインのエディタも開けません（「plugin editors are not available on this system yet」）。MIDI と GUI は Brack 本体の側なので、プラグインホストには関係しません。
  - そのアーキテクチャの開発用ファイルを、ツールチェーンが探す場所に置けば付くはずです（試していません）。
- ARM64 の Linux では、`build` に ARM64 版ができます。先に `cmake/toolchains/linux-x64.cmake` と `linux-x86.cmake` で、x64 版を `build-x64` に、x86 版を `build-x86` にクロスビルドしておくと、ARM64 版がそれらのプラグインホストを同梱します。
- ARM64 版のテストは、x64 の Linux では qemu-user で動きます（`QEMU_LD_PREFIX=/usr/aarch64-linux-gnu ctest --test-dir build-arm64`）。
- RHEL 系（AlmaLinux など）の x64 では、x86 版を、32 ビットのライブラリ（`glibc-devel.i686` など）と `cmake/toolchains/linux-x86-m32.cmake` でビルドできます。クロスビルドではないので、X11 と ALSA の 32 ビットの開発用ファイル（`libX11-devel.i686`、`alsa-lib-devel.i686`）があれば、エディタと MIDI ポートも付きます。

```bash
cmake -S . -B build-x86 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo --toolchain cmake/toolchains/linux-x86.cmake && cmake --build build-x86
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build
cmake -S . -B build-arm64 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo --toolchain cmake/toolchains/linux-arm64.cmake && cmake --build build-arm64
```

### macOS

Xcode（またはそのコマンドラインツール）と、Homebrew の `cmake`（3.25 以上）と `ninja` が要ります。`build/bin/` に Brack.app、brack-cli、libbrack.dylib、プラグインホストができます。

- `cmake --install` では、Linux と同じ配置（`lib` には `libbrack.dylib`）に加えて、一番上に `Brack.app` が入ります。
- Apple シリコンの Mac では、Intel 用（x64）を `build-x64` にクロスビルドできます。先に作っておくと、Apple シリコン用のビルドがその x64 のプラグインホストを同梱します。
- 両方を 1 つにしたユニバーサルなビルドもできます。プログラムと libbrack.dylib は両方入りになり、プラグインホストはアーキテクチャごとに 1 つずつできます。

```bash
cmake -S . -B build-x64 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_OSX_ARCHITECTURES=x86_64 && cmake --build build-x64
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build && ctest --test-dir build
cmake -S . -B build-universal -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" && cmake --build build-universal
```

配るために Hardened Runtime で署名するときは、`-DBRACK_CODESIGN_IDENTITY="Developer ID Application: ..."`（試すだけなら `-`）を付けます。プログラム、プラグインホスト、libbrack.dylib、Brack.app に、プラグインに要る権限（`cmake/brack.entitlements`。ほかの開発元のプラグインの読み込み、JIT）を付けて署名します。公証は別に要ります（Apple の開発者アカウントが要るので、試していません）。

## テストと道具

- ctest には、エンジン、形式ごとのテスト用シンセ、クラッシュの封じ込め、プラグインホスト、DLL、.NET バインディングなどのテストが入っています（設計メモの 8 章）。
  - .NET のテスト（`bindings/dotnet/Brack.Tests`）は、.NET SDK があれば 64 ビット版の ctest に含まれます（Windows、Linux、macOS）。
  - `test_api_parity` は、エンジンの公開メソッドと C API が対応していること、C API の関数が .NET バインディングに漏れなく入っていることを確かめます。
  - `test_isolation` は 1 ブロックあたりの時間も表示します（設計メモの 15 章）。`test_isolation --block-cost <brack-test-synth.clap> [<frames>]` で、ブロックの費用だけを測れます。プラグインを 1・2・4・8 つにして、ブロックを休みなく続ける測り方と、実際のデバイスのようにブロックごとの時刻に処理する測り方（paced）の両方で測ります。`<frames>` を省くと 64 と 256 フレームの両方です。Linux と macOS では、1 ブロックあたりの眠った回数も出します（別の CPU に移った回数は Linux だけ）。
- `test_virtual_midi` と `vmidi_probe` は、実際に仮想ポートを作る手動テストです。Windows MIDI Services の不具合（microsoft/MIDI#1047。仮想ポートを消すとサービスが止まる）があるため、ctest には含めていません。`vmidi_probe --check` は仮想ポートを作らないので、いつ回しても構いません。
- `tools/linux_syntax_check.sh` は、共通コードが警告無しでコンパイルできることを確かめます。クロスコンパイラーは Ubuntu の `g++-i686-linux-gnu` と `g++-aarch64-linux-gnu` です。
  - WSL では Windows 側のファイル（`/mnt/c`）を読むのが遅いので、スクリプトはソースと依存ライブラリを Linux 側（`~/.cache/brack-syntax-check`）に写してからコンパイルします。3 つのアーキテクチャで約 10 秒です（24 コアの PC）。
- `tools/arch_matrix.sh` は、設計メモの 13 章の組み合わせの表を、期待を付けて回します。回せない組み合わせも理由を添えて表に出し、`--all` ではそれも失敗にします。所要時間は Mac で約 15 秒、VM で約 7 秒です。
- `.sh` は、Windows で取り出しても LF のままにします（`.gitattributes`）。Windows の作業フォルダから WSL などの bash で動かすためです。
- `tools/midi-timing` は、MIDI が周期のどこに置かれるかを実際のデバイスで計測します（下の「MIDI の時刻を測る」、設計メモの 15 章）。送ったうちプラグインに届いた数と、先に送ったものより前に届いた数とその幅も出します。音声が途切れても、MIDI を落とさず、順序も崩さないことを確かめるためです。
- `tools/app-icon` は、`src/gui/icon` の SVG から、Windows の `.ico`、macOS の `.icns`、Linux の PNG と、Linux のウィンドウのアイコンの画素（`window_icon.inc`）を書き出します。書き出したファイルもリポジトリに入れます。ビルドに SVG を描く道具を要らないようにするためです。
  - SVG を直したら、リポジトリの最上位で `dotnet run --project tools/app-icon` を実行します。32px 以下のアイコンは細部を省いた `brack-small.svg` から描くので、両方を合わせて直します。

### Windows で

- 構成からやり直したビルドでは、依存ライブラリ（VST3 SDK など）のファイルごとに `cl : コマンド ライン warning D9025 : '/W4' より '/W0' が優先されます` が出ます。全体の `/W4` に依存ライブラリだけ `/W0` を足しているためで、ソースへの警告ではありません。
- x86 版を作り直さずに x64 版だけを作り直すと、x86 版が同梱する x64 のプラグインホストは古いままです。プラグインホストとの約束の版（`kProtocolVersion`）が上がっていれば、x86 版のテスト（`isolation`、`dll`）が「the plugin host is from another build of Brack (build both architectures again)」で失敗します。
- テストは、そのアーキテクチャの環境から回します（x86 版は `vcvars32.bat`、ARM64 の PC の x64 版と x86 版は `vcvarsarm64_amd64.bat` / `vcvarsarm64_x86.bat`）。
- `tools/arch_matrix.sh` は Git Bash で回します（`"C:\Program Files\Git\bin\bash.exe" -lc "cd <リポジトリ> && tools/arch_matrix.sh --all"`）。
  - 先に `powershell -ExecutionPolicy Bypass -File tools\arm64ec_synth.ps1` で、テスト用シンセの ARM64EC 版と ARM64X 版（`build-arm64ec`）を作っておきます（約 1 分）。これには Visual Studio の `VC.Tools.ARM64EC` が要ります。
    - できたものは `dumpbin /headers` で確かめられます。ARM64EC 版は「8664 machine (x64) (ARM64X)」、ARM64X 版は「AA64 machine (ARM64) (ARM64X)」です。
  - スクリプトは ARM64EC の部品を持つ Visual Studio を探し（`vswhere -latest` はその部品の無いものを選ぶことがあるため）、x64 の PC では x64 から ARM64 へのクロスの環境（`amd64_arm64`）で作ります。この 2 つのバンドルが無ければ、表はその行を「not run」にし、`--all` を失敗にします。
- CMake が取ってくる依存ライブラリはフォルダの階層が深いので、長いパスを許しておきます（レジストリの `LongPathsEnabled`、git の `core.longpaths`）。
- SSH や PowerShell のスクリプトから Visual Studio の環境（`vcvarsall.bat`）を作ってビルドするときは、コンソールの入力と出力の両方のコードページを UTF-8 にしてから構成します。出力だけを替えると、Ninja がヘッダーの依存関係を拾わなくなります。
  - cl.exe は `/showIncludes` の接頭辞（「メモ: インクルード ファイル:」）を出力のコードページで出し、CMake は入力のコードページで読むためです。拾っていなければ、`ninja -C build -t deps <obj>` が `#deps 0` になり、ヘッダーを変えても何もビルドし直されません。
  - 接頭辞はビルドフォルダの構成に焼き付くので、直した後はビルドフォルダを消して構成からやり直します。
- PowerShell の `*>` で書いたログは UTF-16 になります。`cmd /c "... > log 2>&1"` で書けば、元の文字コードのままです。
- PowerShell の `-File` に配列を渡すと、1 つの文字列になります（`-Modes now,ahead` は `"now,ahead"`）。スクリプトの中で `-split ","` します。
- SSH から起動した GUI は、画面の無いセッションで動くので見えません。デスクトップに出すには、タスクスケジューラでログオン中のセッションに起動します（`schtasks /create /tn brackgui /tr <brack.exe のパス> /sc once /st 23:59 /it /f`、`schtasks /run /tn brackgui`、終わったら `schtasks /delete /tn brackgui /f`）。
- ARM64 のプログラム向けの OpenGL を持たないディスプレイドライバー（VMware の SVGA 3D など）では、ARM64 版の GUI が「cannot open an OpenGL 3 window」で終わります。Mesa の llvmpipe（ソフトウェアで描く OpenGL）の ARM64 版の `opengl32.dll`（[mmozeiko/build-mesa](https://github.com/mmozeiko/build-mesa) の `llvmpipe-arm64`）を `brack.exe` の隣に置くと開きます（ビルドし直しても消えません）。x64 版と x86 版は、そのままで開きます。Microsoft の「OpenCL, OpenGL, and Vulkan Compatibility Pack」は D3D12 の上で動くので、D3D11 までのドライバー（VMware の SVGA 3D）では効きません。

### Linux で

- テストと計測に使うものは、Ubuntu では `alsa-utils`（`aplaymidi`、`aconnect`）、`dotnet-sdk-10.0`、`xdotool` です。x64 の Linux で x86 版と ARM64 版を動かすには、`libc6-i386`、`qemu-user`、`qemu-user-binfmt` も入れます。エディタを確かめるときは、Ubuntu のパッケージのプラグイン（`dpf-plugins`、`lsp-plugins`）が使えます。
- `tools/arch_matrix.sh` は、qemu-user と FEX-Emu の登録（binfmt_misc）を切り替え、32 ビットの C ライブラリを（そのテストだけのマウントの名前空間で）隠して回します。そのため、パスワード無しの sudo が要ります。終わると元に戻します。
- `test_isolation --block-cost` は、RealtimeKit の要求の回数の上限（ユーザーあたり 20 秒に 25 回）を超えます（`<frames>` を省くと 61 回、渡すと 31 回）。そのため、後のほうのプラグインホストは普通の優先度のままになりえます（警告がログに出ます）。ctest 全体を流した直後も、前のテストが回数を使い切っているので同じです（`Device or resource busy`）。
- `BRACK_TEST_XFT_DPI=1` のときだけ、`test_isolation` が画面の `Xft.dpi` を一時的に変え、エディタに `set_scale` が届くことを確かめます。同じ画面のすべてのプログラムに見えるので、既定では試しません。元の `RESOURCE_MANAGER` はそのまま書き戻します。
- SSH から起動したプログラムは、RealtimeKit に断られて普通の優先度で動きます（[詳しい説明の「リアルタイム優先度（Linux）」](details.ja.md#リアルタイム優先度linux)）。`systemd-run --user` で起動すると RealtimeKit が許します。ユーザーの systemd はセッションの環境（`DISPLAY`、`WAYLAND_DISPLAY`、`XAUTHORITY`）を持っているので、GUI もデスクトップに出ます。実行ファイルは絶対パスで、環境変数は `-E`（`-E BRACK_AUDIO_BACKEND=alsa` など）で渡します。SSH のシェルでは、先に `XDG_RUNTIME_DIR=/run/user/$(id -u)` にしておきます。

  ```bash
  systemd-run --user --unit=brack-gui --collect ~/brack/build/bin/brack
  systemd-run --user --quiet --wait --pipe -p WorkingDirectory=$PWD dotnet tools/midi-timing/bin/Release/net10.0/MidiTiming.dll build/bin/libbrack.so build/bin/brack-test-synth.clap now 480 1500
  ```

- 時計が素直な出力で測るときや出力の音を録るときは、PipeWire に null sink を足して既定にします。録音のプログラムも既定の出力に付いていくので（`pw-record` は `--target` を渡しても `node.dont-move` を付けても付いていく）、自動でつながないようにし、null sink のモニターに手でつなぎます。録ったものは、1 ms ごとのピークで無音を数えます。終わったら既定を戻し、null sink を消します（`pw-cli destroy brack-test-sink`）。

  ```bash
  pw-cli create-node adapter '{ factory.name=support.null-audio-sink node.name=brack-test-sink node.description="Brack test sink" media.class=Audio/Sink object.linger=true audio.position=[FL FR] audio.rate=48000 }'
  wpctl set-default <wpctl status の Brack test sink の番号>
  pw-record -P "{ node.name=rec node.autoconnect=false }" --rate 48000 --channels 2 --format s16 out.wav &
  pw-link brack-test-sink:monitor_FL rec:input_FL
  pw-link brack-test-sink:monitor_FR rec:input_FR
  ```

- 計測の途中でデバイスを止める（Brack が止まったと判断して開き直すかを見る）には、自分のセッションの PipeWire を一時停止させます（`kill -STOP` の 1 秒後に `kill -CONT`）。PulseAudio なら PulseAudio を止めます。
- 作業ツリーを別の機械へ tar で送るときは、展開した時刻を付けて（`tar -xm`）、Ninja にビルドし直させます。git で送るなら、受け側のリポジトリを `receive.denyCurrentBranch updateInstead` にしておくと、push で作業ツリーも更新されます。
- ARM64 の Linux で x64 と x86 のプラグインを動かすには、FEX-Emu を入れます。Ubuntu では、`ppa:fex-emu/fex` から、CPU に合う `fex-emu-armv8.4` などと、登録の `fex-emu-binfmt32`、`fex-emu-binfmt64` を入れます。
  - ルートファイルシステムは `FEXRootFSFetcher` で取ってきます。Ubuntu 24.04 のもの（glibc 2.39）を使います。Ubuntu 26.04 でクロスビルドしたプラグインホストは glibc 2.38 を要求するので、それより古いもの（Ubuntu 22.04 など）では起動しません（設計メモの 15 章の「配るプラグインホストの glibc」）。配布物のプラグインホストは glibc 2.28 から動きます。
  - Fedora 44 のもの（glibc 2.43）では、FEX-Emu の不具合で、x64 と x86 のプラグインが自分のスレッドでスタックをあふれさせても、落ちたと報告されません（回り続けるか、「stopped responding」になります）。そのため、`crash` と `isolation` のテストが失敗します（設計メモの 15 章）。
  - `~/.config/fex-emu/Config.json` が無いと、`FEXRootFSFetcher` は「既定にした」と言うのにそれを書かず、FEX-Emu がルートファイルシステムを見つけられません。先に `{"Config":{"RootFS":"Fedora_44"}}` のように書いておきます（あれば、ツールが書き換えます）。
  - ルートファイルシステムを 1 回ごとに選ぶには、`FEX_ROOTFS` だけでは足りません（動いている FEXServer がマウントしたものが使われます）。`XDG_DATA_HOME` と `FEX_SERVERSOCKETPATH` も分けて、FEXServer を別にします。
  - qemu-user の登録（`qemu-user-binfmt`）は FEX-Emu の登録と競合し、入れるともう一方が外れます。また、qemu-user のローダーとライブラリは `QEMU_LD_PREFIX`（`/usr/x86_64-linux-gnu` か `/usr/i686-linux-gnu`）1 つで渡すので、一度に動くのは x64 か x86 のどちらか一方です。
  - PATH から FEX-Emu を探す経路は、登録を一時的に切って確かめます（`echo 0 | sudo tee /proc/sys/fs/binfmt_misc/FEX-x86_64 /proc/sys/fs/binfmt_misc/FEX-x86`、戻すときは `1`）。`tools/arch_matrix.sh --all` は、これを自分で切り替えて回します。

### MIDI の時刻を測る（`tools/midi-timing`）

```bat
dotnet build tools\midi-timing -c Release
tools\midi-timing\bin\Release\net10.0\MidiTiming.exe build\bin\brack.dll build\bin\brack-test-synth.clap now 480 1500
```

- ツールは 7.3 ms ごと（周期とずらした間隔）にノートを送り、テスト用シンセが `BRACK_TESTSYNTH_FRAMES` で記録した、ノートが届いたフレームを、送った時刻と比べます。
- `now` と `ahead` は Brack の API から送るので、ほかに要るものはありません。`--exclusive` で WASAPI の排他モードになります。音量は 0 で、音は出ません。
- `midi`、`midi-ahead`、`midi-zero` は MIDI ポートから送ります。
  - Windows では、`--port=` の名前の Windows MIDI Services のエンドポイント（ループバックなど）から送ります。無ければ同じ名前の WinMM の出力から送り、どちらも無ければ Brack に仮想ポートを作らせます。
  - Linux と macOS では、ツールが `--port=` の名前の ALSA シーケンサー / CoreMIDI のポートを自分で作り、Brack がそれを入力として開きます。
- Windows MIDI Services が、仮想ポートを消すと止まる版（microsoft/MIDI#1047）なら、loopMIDI などのポートの名前を渡して、仮想ポートを作らせないようにします。WinMM では時刻を付けられないので、その経路では `midi-ahead` は回せません。
- Windows で名前を打ち間違えると、どちらの経路も見つからず、Brack が仮想ポートを作ってしまいます。先に WinMM の出力の一覧で名前を確かめます。
- 揺れの幅は、届いたフレームと送った時刻に当てはめた直線からのずれの幅で、一様な揺れと一度の段差を区別しません。段差が 1 つあると、幅はほぼその高さになり、直線の傾きもずれます。
  - 幅が大きいときは、ずれを時間に沿って並べて、段差か、一様か、ずっと傾いているか（デバイスの時計のずれ）を見分けます。描画位置から steady_clock の進みを引いた値（`ArrivalClock` の読み）を並べると、デバイスがいつ時間を失ったかが分かります。どちらもツールには無いので、足して見ます。

## リリース

GitHub でリリースを作ると、`.github/workflows/release.yml` が Brack をビルドしてテストし、配布物と NuGet パッケージをリリースに添付して、.NET バインディングを nuget.org に公開します。タグは `v1.2.0` の形で、プレリリースは `v1.2.0-beta.1` のように書けます。タグから `v` を除いたものが、`BRACK_VERSION` と NuGet パッケージの版になります。

- 配布物は、Windows（x64、x86、ARM64）と macOS（ユニバーサル）の zip、Linux（x64、ARM64）の tar.gz です。中身は[詳しい説明の「配布物の中身」](details.ja.md#配布物の中身)のとおりです。
- Windows の 3 つは x64 の機械で、「ビルド」の「Windows」の順にビルドします。ARM64 版はクロスビルドなので、CI ではテストしません。
- macOS は Apple シリコンの機械で、ユニバーサルなビルドをアドホック署名（`-`）で作ります。Developer ID の署名と公証はしていません。
- Linux は、glibc 2.28 以降で動くように、manylinux_2_28 のコンテナ（AlmaLinux 8、GCC 14）の中で `tools/linux_release_build.sh` がビルドします。テストはコンテナの外の Ubuntu で動かします。
  - x64 の機械で x86 版（`-m32`）と x64 版を作り、ARM64 の機械で、x64 版の x64 と x86 のプラグインホストを同梱する ARM64 版を作ります。
  - 手で作るときは、スクリプトの先頭のとおり、ソースのフォルダを同じパスでコンテナにマウントして実行します。ARM64 版には、x64 の機械で作った `build/bin` の `brack-host-x64` と `brack-host-x86` を 1 つのフォルダにコピーし、`-DBRACK_OTHER_ARCH_BINS` で渡します。Docker ではフォルダが root の持ち物になるので、後で `sudo chown -R` で戻します（podman のルートレスでは要りません）。
  - 公開する前に、`linux-smoke` が、Ubuntu、Debian、AlmaLinux、Rocky Linux、Fedora、openSUSE、Arch のコンテナで配布物を展開し、`tools/linux_smoke_test.sh` で要るライブラリだけを入れて起動とスキャンを確かめます。README の動作環境の「glibc 2.28 以降」は、これとビルドの環境が根拠です。
- 配布物と NuGet パッケージのファイルは、`tools/check_release_files.sh` の一覧と照らし合わせます。入れるファイルを変えたときは、この一覧も直します。Linux のバイナリは、glibc 2.28 より新しい版を要求していないかも確かめます（`cmake/check_glibc_needs.cmake`）。
- Actions の画面から手で回すときは、タグを渡すと、そのリリースの配布物を作り直して置き換えます。nuget.org に同じ版が既にあれば、パッケージは送りません。タグを渡さなければ試しの実行です。版は `0.0.0-dev.<実行番号>` になり、配布物とパッケージは実行の成果物（artifact）に残すだけです。

## 文書とコメントの書き方

文書は役割で分けます。

| 文書 | 書くこと |
|---|---|
| [README](../README.ja.md) | 使う人向けの、何ができるか、動作環境、インストール、基本の使い方 |
| [details.ja.md](details.ja.md) | 使う人向けの細部（形式、検索場所、クラッシュの扱い、MIDI、オーディオなど） |
| development.ja.md（この文書） | 作る人向けの、構成、ビルド、テストと道具、リリース、作業の注意 |
| [design-notes.md](design-notes.md) | 判断とその理由、調査と計測の結果、残りの作業（TODO） |
| `include/brack/brack.h` と .NET のクラスのコメント | ライブラリの使い方と約束事 |

README、details.md、development.md には英語版（既定）と日本語版（`.ja.md`）があり、片方を直すときはもう片方も合わせて直します。design-notes.md は日本語だけです。

文書にもコメントにも、次のものを書きます。

- 今の作りを選んだ理由と、とらなかった案（とらなかった理由も）。
- 今も効いている制約と回避策（OS や実際のプラグインの癖など）。
- 結論を支える計測値。数値と、主な条件（環境、周期、レート、プラグインの数）を書きます。回ごとの値ではなく、代表の値にします。
- ほかに書いていない、再現や確認の手順。
- 今の約束事と振る舞い。
- 確認の事実は、明らかでない主張の唯一の根拠になるときだけ、短く書きます（`tools/arch_matrix.sh` の行が、確かめる仕掛けを壊すと FAIL になることなど）。

次のものは書きません。

- 直した不具合の経緯（「以前は〜していた」）。今の作りの理由になるときだけ、その理由として書きます。直したときは、経緯を書き足さずに、今の作りの説明を書き換えます。
- 日付付きの確認の記録（「〜で〜件が通った」「〜で確かめた」）。どこで何を確かめたかは、設計メモの 13 章の「アーキテクチャごとの状態」の表だけに書きます。
- 計測の手順の細部（回数、交互の測り方、中央値の取り方、止めたプロセス）。結果の信頼性にかかわるときだけ書きます。測り方そのものは、この文書の「テストと道具」に置きます。
- 結論に関係しない VM の構成。

ほかのファイルやコードのコメントから参照されている見出しと太字のラベル（設計メモの章の番号、「15 章の『Windows（ARM64）』」など）は、名前を変えません。コメントでは、コードを読めば分かることは書きません。

## プラットフォームごとの状態

対象のアーキテクチャは[詳しい説明の「動作環境」](details.ja.md#動作環境)のとおりです。プラグインの実行ファイルは、PE・ELF・Mach-O（ユニバーサルを含む）のどれも、どの OS でもヘッダーからアーキテクチャとエクスポートを読めます。

OS とアーキテクチャごとに、どこまで確かめたか（テストの件数、確かめた環境）は[設計メモの 13 章](design-notes.md#13-他プラットフォームへの対応)の「アーキテクチャごとの状態」の表にあります。残っていること（TODO）とその理由も同じ章にあります。OS ごとの実装の判断と確かめたことは、15 章の OS ごとの節にあります。Linux と macOS は、Brack の POSIX の部分（プラグインホストとの通信など）が共通です。
