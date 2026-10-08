# Brack

[English](README.md) | 日本語

CLAP / VST3 / VST2 のソフトウェア音源をいくつも同時に鳴らすための、シンプルなホストです。MIDI の入力をどの音源につなぐか、音源の出力をどのスピーカーのチャンネルに出すかを、自由に組み合わせられます。

## 特徴

- **MIDI をそのまま渡す** — 受け取った MIDI を変換せずに音源へ渡します（VST3 だけは形式の制約で変換します）。
- **自由なルーティング** — MIDI 入力と音源、音源の出力と出力チャンネルを、多対多でつなげます。
- **音源のサンプリングレートを選べる** — 音源を好きなレート（96 kHz など）で動かし、出力デバイスのレートへ高品質に変換します。
- **仮想 MIDI ポート** — ほかのアプリから Brack の音源へ MIDI を送れるポートを作れます。Windows でも loopMIDI などは要りません。
- **音源が落ちても止まらない** — 音源は 1 つずつ別のプロセスで動くので、1 つが落ちても固まっても、Brack とほかの音源は鳴り続けます。
- **ほかのアーキテクチャの音源** — 64 ビット版の Brack で 32 ビットの音源を、ARM 版で x64 の音源を使えます。
- **3 つの形** — GUI、コマンドライン（CLI）、ほかのアプリに組み込むライブラリとして使えます。

## 動作環境

- Windows（x64・x86・ARM64）。仮想 MIDI ポートには Windows 11 が要ります。
- macOS 14.4 以降（Apple シリコン・Intel）
- Linux（x64・x86・ARM64）。配布物は glibc 2.28 以降で動きます（RHEL 8、Debian 10、Ubuntu 20.04 以降など）。GUI は X11 で動きます（Wayland のデスクトップでは Xwayland で）。

どのアーキテクチャの音源を使えるかは、[詳しい説明](docs/details.ja.md#ほかのアーキテクチャのプラグイン)にあります。

## インストール

配布物は GitHub のリリースのページにあります。OS とアーキテクチャごとに `brack-<バージョン>-<OS>-<アーキテクチャ>` の名前で置いています。

- **Windows** — `brack-<バージョン>-win-x64.zip`（32 ビットの Windows では `win-x86`、ARM では `win-arm64`）を好きな場所に展開し、`bin` の `brack.exe` を実行します。
  - 署名が無いため、初めて実行するときは「Windows によって PC が保護されました」と表示されます。**詳細情報** を押してから **実行** を押してください。展開する前に zip ファイルのプロパティで「許可する」にチェックを入れておくと、出なくなります。
- **macOS** — `brack-<バージョン>-osx-universal.zip`（Apple シリコンと Intel の Mac の両方で動きます）を展開し、`Brack.app` をアプリケーションフォルダに入れます。
  - 初めて開くときは「開けません」と表示されます。**システム設定 → プライバシーとセキュリティ** を開き、下のほうにある「このまま開く」を押すと、次からはふつうに開けます。
- **Linux** — `brack-<バージョン>-linux-x64.tar.gz`（ARM では `linux-arm64`。x86 の配布物は無いので、[ソースからビルド](docs/development.ja.md#linux)します）を好きな場所に展開し、`bin/brack` を実行します。展開したフォルダの中で、ファイルの配置を変えないでください。GUI には X11 と OpenGL のライブラリ（`libX11.so.6`、`libGL.so.1`）が要ります。MIDI ポートとプラグインのエディタに要るものは、[詳しい説明](docs/details.ja.md#配布物の中身)にあります。

アンインストールするには、展開したフォルダ（macOS では `Brack.app`）を削除します。設定は[設定の保存](#設定の保存)の場所に残るので、要らなければそれも削除します。

配布物の中身は[詳しい説明](docs/details.ja.md#配布物の中身)に、ソースからビルドする方法は [docs/development.ja.md](docs/development.ja.md) にあります。

## 使い方

### GUI

画面は左のメニューで **Rack**、**Routing**、**Audio settings**、**Log** に切り替えます。

1. **Rack** の「Add instrument」で音源を追加します。一覧には、見つかった音源（インストゥルメント）が並び、名前やベンダーで絞り込めます。一覧に無いものやエフェクトは「From file...」でファイルを選んで読み込めます。
2. **Rack** の下の MIDI INPUTS で、MIDI の入力（つないだ機器のポートや、Brack が作る仮想ポート）を追加します。
3. **Routing** の表で、どの入力をどの音源につなぐかにチェックを入れます。その下で、音源の出力をどの出力チャンネルに出すかを選びます。追加した音源は、最初から出力 1 と 2 につながっています。
4. 出力チャンネルごとの音量は、**Rack** の音源の欄のフェーダーで決めます（ダブルクリックで 0 dB）。
5. 出力デバイスやサンプリングレートは **Audio settings** で変えます。変えた設定は「Apply」で反映されます。

- 音源の画面は「Editor」ボタンで開きます。
- 全体の音量は、上の「MASTER」フェーダーで調整します（左端でミュート、ダブルクリックで 0 dB）。
- 音源の名前はダブルクリックか「...」メニューの「Rename」で変えられます。音源と MIDI 入力の並びは、ドラッグで変えられます。
- **Log** の「Copy all」で、ログをクリップボードにコピーできます。

### 設定の保存

GUI は、組んだラック（音源とその状態、MIDI 入力、ルーティング、オーディオ設定）とウィンドウの位置などを、次の場所の `settings.json` に自動で保存し、次に起動したときに元に戻します。

| OS | 場所 |
|---|---|
| Windows | `%APPDATA%\brack\` |
| macOS | `~/Library/Application Support/brack/` |
| Linux | `~/.config/brack/`（`XDG_CONFIG_HOME` が設定されていればそちら） |

- ラックは、Save でセッションファイル（JSON）にも保存でき、Open で開けます。起動するときにセッションファイルを指定すると（`brack session.json`）、保存したラックの代わりにそれを開きます。
- `--config <フォルダ>` を付けて起動すると、上の場所の代わりにそのフォルダに設定を保存し、そこから読みます。試しに動かすときなど、いつもの設定に触れたくないときに使います。

保存の細かい規則と、ラックをほかの OS に持っていく方法は、[詳しい説明](docs/details.ja.md#設定の保存)にあります。

### 音源が落ちたとき

落ちた音源は「Crashed」と表示されて無音になり、Brack とほかの音源は鳴り続けます。どこで落ちたかは Log に出ます。「Reload」を押すと、落ちる前の状態から読み込み直せます。

### 仮想 MIDI ポート

Rack の MIDI INPUTS で仮想ポートを追加すると、ほかのアプリ（MIDI プレイヤーなど）から、その名前の MIDI 出力先として見えます。そこへ送った MIDI が Brack の音源に届きます。

> **Windows の既知の問題:** Windows MIDI Services には、仮想ポートを削除すると MIDI のサービスが止まり、再起動するまでどのアプリも MIDI ポートを開けなくなる不具合があります（microsoft/MIDI #1047。2026 年 11 月下旬の Windows 更新で直る予定です）。Brack を終了したときも仮想ポートは削除されるので、同じことが起きます。該当する Windows では、Brack が仮想ポートの追加欄で警告します。32 ビット版の Brack では、仮想ポートは使えません。

### Linux で音が途切れるとき

Brack はオーディオをリアルタイム優先度で動かします。デスクトップから起動すれば、ふつうは何もしなくて構いません。SSH から起動したときや、音源が多いときにリアルタイムにならないことがあります。そのときの設定は[詳しい説明](docs/details.ja.md#リアルタイム優先度linux)にあります。

### CLI

```bat
brack-cli devices                     :: 出力デバイスの一覧
brack-cli midi-inputs                 :: MIDI 入力の一覧
brack-cli scan                        :: 見つかったプラグインの一覧
brack-cli play Synth.clap --virtual "Brack" --rate 96000 --gui --save my.json
brack-cli run my.json                 :: セッションを鳴らす（Ctrl+C で終了）
```

オプションの一覧は `brack-cli help` で表示します。

### もっと詳しく

[docs/details.ja.md](docs/details.ja.md) に、次のことを書いています。

- 使えるプラグインの形式と、プラグインを探す場所
- 音源が落ちたときに守られる範囲と、音源を Brack と同じプロセスで動かす設定
- サンプリングレート変換の品質と遅延
- 出力デバイスを抜いたときと、既定のデバイスが変わったときの動き
- MIDI がそのまま渡らない場合（VST3 など）と、OS ごとの MIDI ポートの扱い

## アプリに組み込む

Brack は、ほかのアプリからライブラリとしても使えます。ライブラリからは、MIDI ポートを介さずに API で直接 MIDI を流し込めます。GUI でできるラックの操作は、すべてライブラリからもできます。

- **C API。** `brack.dll`（Linux では `libbrack.so`、macOS では `libbrack.dylib`）と、ヘッダーの [`include/brack/brack.h`](include/brack/brack.h) です。使い方、スレッドの扱い、配布するときに一緒に置くプラグインホストについては、ヘッダーの冒頭とそれぞれの関数のコメントに書いてあります。
- **.NET バインディング（.NET 10）。** [`bindings/dotnet/Brack`](bindings/dotnet/Brack) です。C API の上に C# らしいクラス（`BrackEngine`、`BrackLibrary`）を用意しています。NuGet パッケージ（`dotnet pack bindings/dotnet/Brack`）としても、プロジェクト参照としても使えます。どちらも、ビルド済みのネイティブライブラリとプラグインホストを OS とアーキテクチャごとに含み、実行中の OS とアーキテクチャに合うものを読み込みます。使い方はクラスのコメントに、パッケージに入るものは `Brack.csproj` のコメントにあります。

## 開発者向け

ビルドの詳細（アーキテクチャごとのビルド、オプション、テスト、道具）と構成は [docs/development.ja.md](docs/development.ja.md) に、設計の判断と調査の結果は [docs/design-notes.md](docs/design-notes.md) にあります。

## ライセンス

Brack は MIT License で公開しています（[LICENSE](LICENSE)）。

Brack が使っている第三者のソフトウェアのライセンスは [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) にあります。配布物にも、同じファイルが入っています。

VST は Steinberg Media Technologies GmbH の商標です。
