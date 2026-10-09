# Brack の詳しい説明

[English](details.md) | 日本語

[README](../README.ja.md) の補足です。動作環境の細部、配布物の中身、設定の保存の規則、プラグインとオーディオと MIDI の扱いを説明します。ビルドの方法は [development.ja.md](development.ja.md)、設計の判断は [design-notes.md](design-notes.md) にあります。

## 動作環境

| OS | アーキテクチャ | オーディオ | MIDI |
|---|---|---|---|
| Windows | x64、x86、ARM64 | WASAPI（共有モード、排他モード） | MIDI 入力ポート（WinMM）、仮想ポート（Windows 11 の Windows MIDI Services） |
| Linux | x64、x86、ARM64 | PulseAudio（PipeWire でも）、ALSA、JACK | ALSA シーケンサー |
| macOS 14.4 以降 | Apple シリコン、Intel（x64） | Core Audio | CoreMIDI |

- Linux の GUI とプラグインのエディタは X11 で動きます（Wayland のデスクトップでは Xwayland で）。
- Intel 用の macOS 版は、Apple シリコンの Mac の Rosetta 2 で動作を確かめています。Intel の Mac ではまだ確かめていません。

### ほかのアーキテクチャのプラグイン

各アーキテクチャの Brack が動かせるプラグインは次のとおりです。

| Brack | 動かせるプラグイン |
|---|---|
| Windows x64 版、x86 版 | x86、x64 |
| Windows ARM64 版 | x86、x64（Windows のエミュレーション）、ARM64 |
| macOS Apple シリコン版 | ARM64、x64（Rosetta 2 が入っているとき） |
| macOS Intel 版 | x64 |
| Linux x64 版 | x64、x86（32 ビットの C ライブラリがあるとき） |
| Linux x86 版 | x86 |
| Linux ARM64 版 | ARM64、x64 と x86（FEX-Emu か qemu-user があるとき） |

- その PC で動かせないアーキテクチャのプラグインを読み込もうとすると、「this computer does not run x64 programs」のように報告します。スキャンでは、そのようなプラグインを黙って飛ばします。
- Linux ARM64 版は、x64 と x86 のプラグインを、カーネルに（binfmt_misc で）登録された FEX-Emu で動かします。登録が無ければ、PATH にある `FEX`（旧名 `FEXInterpreter`）で起動します。qemu-user の登録（`qemu-x86_64`、`qemu-i386`）も、x64 や x86 のプログラムのローダーが `/` か `QEMU_LD_PREFIX` の下にあれば使います。
- FEX-Emu は設定を `$XDG_CONFIG_HOME/fex-emu` から読みます。`XDG_CONFIG_HOME` を変えて Brack を起動すると、x64 と x86 のプラグインが一覧に出なくなります（そのフォルダに `fex-emu` へのリンクを置けば出ます）。
- ほかのアーキテクチャのプラグインは、そのアーキテクチャのプラグインホストが動かします。配布物には、上の表のプラグインホストが入っています（ソースからビルドするときは [development.ja.md](development.ja.md) の「同梱するプラグインホスト」）。

## 配布物の中身

- **Windows** — `bin` に GUI（`brack.exe`）、CLI（`brack-cli.exe`）、ライブラリ（`brack.dll`）と、ほかのアーキテクチャのプラグインを動かすプラグインホストが入っています。`include` と `lib` には、アプリに組み込むためのヘッダーとインポートライブラリが入っています。
- **macOS** — `Brack.app` は、プラグインホストを自分の中（`Contents/MacOS`）に持ちます。CLI とライブラリ（`libbrack.dylib`）は、同じ zip の `bin` と `lib` にあります。
- **Linux** — `bin` に GUI（`brack`）と CLI（`brack-cli`）、`lib` にライブラリ（`libbrack.so`）、`libexec/brack` にプラグインホストが入っています。プログラムと `libbrack.so` は、プラグインホストを自分の隣と `../libexec/brack` から探します。
  - glibc 2.28 以降の Linux で動きます（musl の Linux では動きません）。C++ の実行時ライブラリは静的にリンクしているので要りません。GUI には `libX11.so.6` と `libGL.so.1` が要ります。
  - `libasound.so.2` と `libX11.so.6` は、使うときに読み込みます。そのため、ライブラリ（`libbrack.so`）と CLI は、それらの無いサーバーやコンテナでも動きます。`libasound.so.2` が無ければ MIDI ポート（「MIDI ポート」の「Linux」）が、`libX11.so.6` が無ければプラグインのエディタが使えません。
  - x64 版の x86 のプラグインホストには、32 ビットの C ライブラリが要ります（Ubuntu では `sudo dpkg --add-architecture i386` の後に `libc6:i386`）。x86 のプラグインのエディタには、32 ビットの `libX11.so.6`（`libx11-6:i386`）も要ります。
  - ARM64 版の x64 と x86 のプラグインホストは、x64 版と同じものです。x64 と x86 のプラグインのエディタは、FEX-Emu や qemu-user が使う x86 のシステムに、そのアーキテクチャの libX11 があるときだけ開けます（無ければ「plugin editors are not available without libX11.so.6」）。
  - `share/icons` と `share/applications/brack.desktop` は、`/usr/local` のような、`bin/brack` が `PATH` に入る場所に展開したとき、デスクトップのメニューに Brack を出すためのものです。
- ライセンス（`LICENSE`）と、組み込んだライブラリのライセンス（`THIRD_PARTY_NOTICES.md`）は、Windows と macOS では zip を展開したフォルダの最上位に、Linux では `share/doc/brack` に入っています。

## 概念

| 要素 | 説明 |
|---|---|
| プラグイン | CLAP / VST3 / VST2 のインスタンス。ID（例: `lead`）で識別します。ノート入力ポートと音声出力ポートを持ちます。形式はファイルの拡張子で決まります（`.clap`、`.vst3`、VST2 は Windows では `.dll`、Linux では `.so`、macOS では `.vst`）。 |
| MIDI ソース | `hardware`（既存の MIDI 入力ポートを名前で開く）、`virtual`（Brack が公開する仮想ポート）、`api`（DLL / API からのみ入力） |
| MIDI ルート | ソースからプラグインのノートポートへ接続します。多対多で接続できます。 |
| 音声ルート | プラグインの出力ポートのチャンネルから出力チャンネルへ、ゲイン付きで接続します。 |
| マスターボリューム | 全出力チャンネルに、サンプリングレート変換（SRC）の後でかかるゲインです。 |
| セッション | 上記すべてと、プラグインの状態（base64）、オーディオ設定を JSON ファイルに保存します。 |

## GUI

- 音源の名前を変えても、変わるのは表示名だけです。ルーティング、セッション、API が使う ID は変わりません。空にすると、プラグイン自身の名前に戻ります。
- 音源と MIDI 入力の並び順は、表示とセッションのためだけのもので、ルーティングと処理には影響しません。
- macOS では、Audio settings の排他モードは無視します。

「Add instrument」の一覧には、スキャンで見つかった音源（インストゥルメント）だけが、形式（CLAP、VST3、VST2）の印を付けて並びます。エフェクトは並びません。

- 「From file...」では、`.clap`、`.vst3`、VST2 のファイルを直接選べます。エフェクトも読み込めます。
- VST3 のバンドルはフォルダなので、中のファイル（`Contents\x86_64-win\*.vst3`、Linux では `Contents/x86_64-linux/*.so`）を選ぶと、そのバンドルを読み込みます。macOS では、バンドルをそのまま選べます。
- Brack と違うアーキテクチャのプラグインは、一覧とラックの両方で、アーキテクチャ（`x86` など）の印を添えて表示します。

Linux では、ファイルダイアログをデスクトップのポータル（xdg-desktop-portal）経由で開きます。Flatpak などのサンドボックスの中でも使えます。ポータルが無ければ zenity（GNOME）を、それも無ければ kdialog（KDE）を使います。どれも無ければ開けず、そのことを Log に出します。最初に開く場所は、同じ種類のファイルを最後に選んだフォルダです（まだ無ければホームフォルダ）。

## CLI

```bat
brack-cli devices                     :: 出力デバイス一覧
brack-cli midi-inputs                 :: MIDI 入力一覧
brack-cli scan [--dir D]              :: プラグイン一覧（各形式の標準パスと環境変数、D）
brack-cli virtual-check               :: 仮想 MIDI ポートを公開できるか
brack-cli play Synth.clap --virtual "Brack" --rate 96000 --gui --save my.json
brack-cli play "C:\Program Files\Common Files\VST3\Synth.vst3" --plugin-id 0123456789ABCDEF0123456789ABCDEF
brack-cli run my.json                 :: セッションを実行（Ctrl+C で終了）
```

- オーディオ関連のオプション（`--device`、`--exclusive`、`--buffer`、`--rate`、`--block`、`--quality`、`--in-process`、`--load-serially`、`--duration`）は、`run` と `play` で共通です。一覧は `brack-cli help` で表示します。
- `play` の `--gui` で、プラグインのエディタを開きます。Linux では `DISPLAY` が要ります。
- `scan` は、Brack と違うアーキテクチャのプラグインに、そのアーキテクチャを添えます。

## 設定の保存

保存するのは、ウィンドウの位置・サイズ・最大化状態、追加のプラグインフォルダ、最後に開いた／保存したセッションファイルのパス、ラックの状態です。ラックの状態には、プラグインとその状態、MIDI 入力、ルーティング、オーディオ設定が含まれます。

- 起動時にセッションファイルを指定すると、保存したラックの代わりにそのファイルを開きます。
- 保存したラックを読めなかったときは、そのラックを上書きしません。その起動で組んだラックは保存せず、ウィンドウの位置などだけを保存します。
- `--config` に渡したフォルダは、プラグインのスキャンキャッシュ（`plugin-cache.json`）にも使います。使えないフォルダを渡したときは、いつもの設定フォルダを使わずに、起動をやめます。いつもの設定を上書きしないためです。
- セッションファイルのパスは、ラックを復元できたときだけ戻します。復元に失敗したときは「untitled」で始まります。そのため、Save で前のセッションファイルを空のラックで上書きすることはありません。
- 終了時のほか、変更から 3 秒間ほかの変更が無ければ自動で保存します。変更が続いても、最初の未保存の変更から 30 秒たてば保存します。Brack が強制終了しても、失うのは直前の数秒分（長くて 30 秒分）です。プラグインが受け取る MIDI は変更に数えません。
- 壊れた設定ファイルは、名前の後ろに `.broken` を付けて退避し、無視します。
- どのアーキテクチャのビルドも同じファイルを使います。32 ビット版で保存したラックを、64 ビット版でそのまま開けます。
- ラック（セッションファイルや `settings.json` の `session`）は、ほかの OS にも持っていけます。プラグインの状態にはパスが入っていないので、プラグインのパスだけをその OS のものに替えます。
  - Windows で使っていた MIDI 入力ポート（loopMIDI など）は、Linux と macOS では同じ名前の仮想ポート（`kind` を `virtual`）に替えると、ほかのプログラムからそこへ MIDI を送れます。
  - `settings.json` のウィンドウの位置と `plugin-cache.json` は、元の PC の画面とパスに合わせたものなので、持っていきません。
- 設定の保存と読み込みは GUI だけが行います。CLI と DLL は読み書きしません。

プラグインのスキャン結果は、同じ場所の `plugin-cache.json` に保存します。次回のスキャンでは、サイズと更新日時が変わっていないファイルを読み込まず、この結果を使います。読み込みに失敗したファイルは保存せず、毎回調べ直します。

## プラグインの形式と検索場所

| 形式 | ファイル | ファイル内のプラグイン ID |
|---|---|---|
| CLAP | `.clap`（macOS ではバンドルフォルダ） | CLAP の ID（例: `com.vendor.synth`） |
| VST3 | `.vst3`（バンドルフォルダ。Windows では 1 つのファイルも） | クラス ID（32 桁の 16 進） |
| VST2 | Windows では `.dll`、Linux では `.so`、macOS では `.vst`（バンドルフォルダ） | ユニーク ID（4 文字。表示できない場合は `0xXXXXXXXX`） |

標準の検索場所は次のとおりです。

| 形式 | Windows | Linux | macOS |
|---|---|---|---|
| CLAP | `%CommonProgramFiles%\CLAP`、`%LOCALAPPDATA%\Programs\Common\CLAP` | `~/.clap`、`/usr/lib/clap` | `/Library/Audio/Plug-Ins/CLAP`、`~/Library/Audio/Plug-Ins/CLAP` |
| VST3 | `%CommonProgramFiles%\VST3`、`%LOCALAPPDATA%\Programs\Common\VST3` | `~/.vst3`、`/usr/lib/vst3` | `/Library/Audio/Plug-Ins/VST3`、`~/Library/Audio/Plug-Ins/VST3` |
| VST2 | レジストリの `VSTPluginsPath`（`HKLM\SOFTWARE\VST`、`HKCU\SOFTWARE\VST`）、`%ProgramFiles%\VSTPlugins`、`%ProgramFiles%\Steinberg\VSTPlugins`、`%CommonProgramFiles%\VST2`、`%CommonProgramFiles%\Steinberg\VST2` | `~/.vst`、`/usr/lib/vst` | `/Library/Audio/Plug-Ins/VST`、`~/Library/Audio/Plug-Ins/VST` |

- どの OS でも、環境変数 `CLAP_PATH`、`VST3_PATH`、`VST_PATH` に並べたフォルダも探します（区切りは Windows では `;`、Linux と macOS では `:`）。
- Windows では、`%ProgramFiles%` と `%CommonProgramFiles%` は、64 ビットのもの（`C:\Program Files`）と 32 ビットのもの（`C:\Program Files (x86)`）の両方を探します。`HKLM` のレジストリも、64 ビット側と 32 ビット側（`WOW6432Node`）の両方を見ます。どのアーキテクチャのビルドも同じ場所を探します。
- VST2 はシェルプラグイン（1 つの DLL に複数のプラグイン）に対応しています。状態は、チャンクに対応していればバンクチャンクを、対応していなければ現在のプログラム番号と全パラメーターを保存します。
- VST3 では、エディタで操作したパラメーターを処理側へ、処理側の変更（MIDI CC など）をエディタへ反映します。
- Brack にはトランスポートがありません。VST3 には停止中・120 BPM・4/4 の再生情報を渡します。VST2 の時刻情報の問い合わせには応答しません。

## プラグインのクラッシュ

プラグインは 1 つずつ、Brack とは別のプロセス（プラグインホスト `brack-host-<アーキテクチャ>`）で動きます。プラグインがどんな形で落ちても、止まったままになっても、終わるのはそのプロセスだけです。Brack とほかのプラグインは動き続けます。

- 落ちたプラグインは「Crashed」と表示され、無音になり、エディタも閉じます。どの処理で、どのモジュールのどこで落ちたかを Log に記録します（例: `crashed in process: access violation (0xC0000005) at Plugin.dll+0x1234`、止まったときは `stopped responding in process`）。
- 返ってこない処理は、音声処理なら 2 秒、状態の保存・読み込みと生成なら 60 秒、活性化とエディタを開く処理なら 30 秒、そのほかは 10 秒で打ち切ります。
- セッションには、そのプラグインが最後に保存できた状態を書き込みます。GUI の「Reload」（DLL では `brack_reload_plugin`）で、その状態から新しいプロセスで読み込み直せます。ID、名前、ルーティングはそのままです。
- Brack が終わると、強制終了されたときも含めて、プラグインホストもすべて終わります。
- セッションを開くと、プラグインの読み込みと活性化を一斉に行います。時間のかかるプラグインがあっても、ほかのプラグインを待たせません。
  - 2 つ同時に読み込むと失敗するプラグインがあります。SOUND Canvas VA は、2 つ目が「Parameter file1 read error」のダイアログを出して止まるか、状態の読み込みで落ちます。そのときは「Load plugins one at a time」（GUI の設定、CLI の `--load-serially`、DLL の `load_plugins_serially`）をオンにすると、1 つずつ読み込んで活性化します。開くのは遅くなります。
- プラグインごとにプロセスが 1 つ増えるので、読み込みにはプロセスの起動の分だけ時間がかかります。音声はブロックごとにプロセス間でやり取りし、プラグインどうしは並列に動きます。

「Run plugins inside Brack」（GUI の設定、CLI の `--in-process`、DLL の `plugins_in_process`）をオンにすると、プラグインを Brack と同じプロセスで動かします（別のアーキテクチャのプラグインは除きます）。読み込みは速くなりますが、封じ込めは次の範囲に限られます。

- Brack からの呼び出し中のクラッシュ（アクセス違反、Linux ではセグメンテーション違反など。スタックあふれも）は捕まえ、そのプラグインを以後呼び出しません。使い続けるには、セッションを保存して Brack を再起動します。
- プラグインのファイルの読み込み・中身の一覧・解放でのクラッシュも封じ込めます。そのファイルは、Brack を再起動するまで読み込みを拒否します。
- プラグインが自分で作ったスレッドやウィンドウ処理の中でのクラッシュ、即時終了（`abort()` など）、止まったままの処理は、Brack ごと終わらせます。Linux では、プラグインから漏れた C++ の例外も同じです。被害を小さくするため、GUI は設定を自動保存します。

## サンプリングレート変換

変換には r8brain-free-src（線形位相）を使います。

`processSampleRate`（GUI の Audio settings、CLI の `--rate`）を 0 以外にすると、プラグインはそのレートで動き、出力はデバイスのレートへ変換されます（例: プラグイン 96 kHz、デバイス 48 kHz）。WASAPI 共有モードでは、デバイスを常にミックスレートで開くので、OS 側のリサンプラーは使われません。

| 品質 | 阻止域減衰 | −3 dB 点 | 遅延の目安（96→48 kHz / 48→44.1 kHz） |
|---|---|---|---|
| `standard` | 約 136 dB | 20.5 kHz | 4.6 ms / 9 ms |
| `high`（既定） | 約 180 dB | 21 kHz | 4.2 ms / 18 ms |
| `ultra` | 約 207 dB | 21.5 kHz | 9 ms / 17 ms |

どの品質でも、18 kHz までの偏差は 0.01 dB 未満です。整数比の変換（96→48 kHz など）は低遅延です。44.1 kHz 系と 48 kHz 系をまたぐ変換は遅延が大きくなります。

遅延が品質の順に並ばないのは、r8brain がフィルターを FFT のブロックごとにかけるためです。遅延はおよそ「フィルターの長さで決まる 2 のべき乗のブロック長から、フィルターの長さの半分を引いたもの」になります。フィルターが長くなっても同じブロック長に収まるうちは遅延が少し縮み、ブロック長が倍になると遅延も倍近くに延びます。

## 出力デバイスの切断と切り替え

- 出力デバイスが切断されても、処理は止まりません。Brack 自身の時計で実時間どおりに処理を続け、その間の出力は捨てます。デバイスが戻れば、その時点から音が出ます。切断中にたまった MIDI がまとめて流れることもありません。
- 出力が「システムの既定」なら、OS の既定デバイスが変わると約 1 秒以内にそのデバイスへ切り替えます。切断で既定が別のデバイスに移った場合も同じです。Linux の PipeWire ではサウンドサーバーがすぐに切り替えるので、Brack は開き直しません。
- デバイスを名前で指定している場合は、そのデバイスが戻るまで待ち、戻ったら開き直します。
- 切り替えではプラグインを止めません。新しいデバイスのサンプリングレートが違えば、プラグインは今のレートのまま動き、Brack の SRC が新しいレートへ変換します。

Windows では、オーディオのスレッドを MMCSS の「Pro Audio」タスクに入れます。macOS では time-constraint（リアルタイム）で動かします。Linux は下の「[リアルタイム優先度（Linux）](#リアルタイム優先度linux)」のとおりです。

## オーディオ（Linux）

- 出力は、PulseAudio、ALSA、JACK の順に試し、最初に開けたものに出します。PipeWire の環境では、PipeWire の PulseAudio 互換サーバーに出ます。環境変数 `BRACK_AUDIO_BACKEND`（`pulseaudio`、`alsa`、`jack`）で選ぶこともできます。
- デバイスはサウンドサーバー（PipeWire など）のレートで動きます。WASAPI の排他モードは無いので、Windows で保存したセッションの排他モードの指定は無視します。

## リアルタイム優先度（Linux）

Brack は、オーディオのスレッド（Brack の再生スレッドと、プラグインホストのもの）をリアルタイム優先度で動かします。

- rtprio の上限があれば `SCHED_FIFO` にします。無ければ、RealtimeKit に頼んで `SCHED_RR` にします。
- RealtimeKit が許すのは、デスクトップから起動した Brack です。SSH から起動したものやサービスとして動かしたものは断られます。SSH からは、`systemd-run --user` で起動すると許されます（`systemd-run` には実行ファイルを絶対パスで渡します）。
- RealtimeKit がリアルタイムにするのは、既定でユーザーあたり 15 プロセスまでです。デスクトップもこの枠を使うので、Ubuntu 24.04 の GNOME では、リアルタイムにできたプラグインホストは 9 個ほどでした。プラグインが多いときや RealtimeKit が使えないときは、rtprio の上限を設定します。

  ```bash
  echo '@audio - rtprio 95' | sudo tee /etc/security/limits.d/audio.conf
  sudo usermod -aG audio $USER
  ```

  ログインし直すと効きます。
- リアルタイムにできなければ普通の優先度で動き、そのことを Log に出します（例: `plugin hosts' audio threads: normal priority (...)`）。

## MIDI がそのまま渡らない例外

受け取った MIDI は、ふつうはバイト列のままプラグインへ渡します（CLAP では `CLAP_EVENT_MIDI` / `CLAP_EVENT_MIDI_SYSEX`、VST2 では MIDI / SysEx イベント）。例外は次のとおりです。

- CLAP: MIDI ダイアレクトに対応したポートには、すべてのメッセージをそのまま渡します。ノートポートが CLAP ノートダイアレクトしか受け付けない場合に限り、ノートオン／オフを `CLAP_EVENT_NOTE_ON/OFF` に置き換えて渡します。このダイアレクトに対応するイベントが無いメッセージは届きません。
- VST2: すべてのメッセージをそのまま渡します。
- VST3: 形式の制約で、次のように変換します。
  - ノートオン／オフ、ポリフォニックキープレッシャー: VST3 のノートイベント
  - SysEx: そのままのバイト列をデータイベントとして
  - コントロールチェンジ、ピッチベンド、チャンネルプレッシャー、プログラムチェンジ: プラグインが `IMidiMapping` で割り当てたパラメーターの変更として。割り当てが無いメッセージは届きません。
  - システムコモン／リアルタイムメッセージ: 届きません。

## MIDI ポート

### Windows

MIDI 入力の一覧には、WinMM の入力ポートが並びます。入力元が切断されても、一覧に戻ればつなぎ直します。

仮想ポートは、Windows 11 の Windows MIDI Services に仮想デバイスとして作ります。ほかのアプリからは、指定した名前の WinMM / WinRT の MIDI 出力ポートとして見えます。SDK ランタイムやドライバーの追加インストールは要りません。受け取ったメッセージは、MIDI 1.0 のバイト列へ損失なく戻します（SysEx は F0..F7 にまとめ、ノートオンのベロシティ 0 も書き換えません）。

- 32 ビット版では仮想ポートを使えません。Windows MIDI Services のクライアントが 64 ビット版しか無いためです（`brack-cli virtual-check` が理由を表示します）。MIDI 入力ポートと API からの入力は使えます。32 ビットのプラグインに仮想ポートから入力したいときは、64 ビット版の Brack を使ってください（32 ビットのプラグインも動きます）。

> **既知の問題:** Windows MIDI Services には、仮想デバイスを削除するとサービスが止まる不具合があります（microsoft/MIDI #1047）。修正は、2026 年 11 月下旬の Windows 更新で配られる予定です。止まると、再起動するまで、システム全体で MIDI ポートを開く・閉じる処理ができなくなります。仮想ポートが 2 つ以上あると、最初のポートの削除で止まり、残りのポートは削除されずに残ります。
>
> - Brack は、該当する Windows を見つけると、GUI（仮想ポートの追加欄）と `brack-cli virtual-check` で警告を出します。
> - サービスが止まったことに気づくと、残りのポートの解放を待たずにすぐ終了します。新しい仮想ポートの作成もすぐにエラーにします。

### Linux

ALSA シーケンサーを使います。機器のポートも、ほかのプログラムのポート（仮想キーボード、シーケンサーなど）も同じ一覧に並びます。PipeWire の環境でもそのまま使えます。

- MIDI 入力の一覧には、MIDI を送るポートがポートの名前で並びます。ほかのクライアントのポートと名前が重なるときだけ、クライアントの名前を括弧で添えます（`Out (VMPK)` など）。クライアントの名前も同じポート（同じ機種のインターフェース 2 台など）は、2 つめから「名前 (2)」のように並びます。保存した名前は、クライアントの名前の括弧があっても無くても見つかります。Brack 自身のポートは並びません。
- 入力元が消える（機器を外す、プログラムが終わる、`aconnect -d` で切られる）と、そのことを知らせます。一覧に戻ればつなぎ直します。
- 仮想ポートは、「Brack」というクライアントの、指定した名前のポートになります。ほかのプログラムからは MIDI の送り先として見えます（`aconnect -l`、`aplaymidi -p` など）。Windows のような削除の不具合はありません。
- ALSA のライブラリ（`libasound.so.2`）が無ければ、MIDI 入力の一覧は空になり、ポートも仮想ポートも開けません（「MIDI ports are not available without libasound.so.2」）。API から送る MIDI は使えます。

### macOS

CoreMIDI を使います。

- MIDI 入力の一覧には、CoreMIDI のソース（機器、IAC ドライバー、ほかのプログラムの仮想ソース）が表示名で並びます。同じ名前のソース（同じ機種の鍵盤 2 台など）は、2 つめから「名前 (2)」のように並びます。
- 入力元が消えると、そのことを知らせます。一覧に戻ればつなぎ直します。
- 仮想ポートは、指定した名前の CoreMIDI の送り先になります。ほかのプログラムからは MIDI の出力先として見えます。
