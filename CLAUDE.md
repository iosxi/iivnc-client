# iivnc-client の作業方針

## リリース運用

**手順は共通の `~/.claude/CLAUDE.md`「修正が終わったら、リリースまで通す」に従う。**
ここにはこのリポジトリ固有の事情だけを書く。

- リモート: `https://github.com/iosxi/iivnc-client.git`(`iosxi/iivnc-client`)
- ブランチ: **`master`**
- 最新バージョンの確認: `git tag --sort=-v:refname | head -1`
- リリースの添付物: **`iivnc-client.exe`**。改名せず、そのまま `gh release create` に渡す。
- バージョン: タグの `vN` とは別に、`src/iivnc-client.rc` の VERSIONINFO、
  `src/iivnc-client.manifest` の `assemblyIdentity`、`src/iivncc.h` の `APP_VERSION` がある。
  機能が変わったら全部上げる。

### exe を変更したとき

ソースを直したら **`build.bat` で exe を作り直してからコミットする**。exe はリポジトリに追跡させている。
`build.bat` は `fxc` で `src/view.hlsl` をバイト列(`build/shader_*.h`)にして埋め込む。
アイコンは `python tools/make-icon.py`。

### iivnc-server と同じファイル

`src/zlite.h` `src/zdeflate.c` `src/zinflate.c` `src/vncdes.c` `src/vncdes.h` `src/theme.c`(theme.c は
先頭の `#include` だけ違う)は `../iivnc-server/src` と中身をそろえる。検証プログラムはサーバー側の
`tools/` にある。

## 動作確認について

検証には `../iivnc-server` の exe と `tools/`(`test.py` の起動・停止、`rfbcheck.py` の受け手)を使う。
サーバーは `-testsrc`(合成した絵。入力はログに書くだけ)で動かすので、利用者の画面は写さず、入力も再現しない。

- **`python tools/clientcheck.py`**: クライアントを `-dump <bmp> -idleexit 1500` で動かし、受け取った絵を
  rfbcheck が Raw で受けた絵と比べる(止まった絵 4 画質、動く絵、途中の大きさの変更)。
  検証用の引数を付けたときは窓を前面に出さない(`SW_SHOWNOACTIVATE`)。
- **`python tools/inputcheck.py`**: クライアントの窓へ `PostMessage` で `WM_KEYDOWN` / `WM_MOUSEMOVE` などを
  直接送り(利用者のキーボード・マウスは使わない)、サーバーのログの `[dryrun-keysym]` 行(キーシムと
  QEMU の番号)と `[dryrun] mouse` 行を期待値と比べる。
- **`python tools/interop.py --port 5911`**: ほかのサーバーにつなぎ、`-encoding raw|rre|hextile|zrle|tight` で
  エンコーディングを 1 つに絞って、受け取った絵を比べる。正解はカーソルの疑似エンコーディングも付けて取る
  (付けないと、サーバーがカーソルを絵に描き込むので 16×16 だけ違う)。
- 通しの速さ: サーバーを `-testsrc video -testfps 0` で動かし、`-exitafter N` で終わらせると、
  ログの「検証の終わり」に回/秒・受信量・1 回の受信と復号の時間が出る。

### 他社のサーバー(TigerVNC Xvnc)を WSL で動かす方法

WSL の AlmaLinux-9 に**何も入れずに**動かした(2026-10-04)。

1. TigerVNC の Linux 版(`tigervnc-1.16.2.x86_64.tar.gz`)を `/tmp/iivnc-test` に展開。
2. 足りない共有ライブラリと xkb は `dnf download --resolve`(入れずに RPM を落とすだけ)で取り、
   `rpm2archive - < x.rpm | tar xzf -` で `/tmp/iivnc-test/libroot` に展開(cpio は無い。rpm2archive の出力は gzip)。
3. Xvnc は xkbcomp を `/usr/bin` から、配列の定義を `/usr/share/X11/xkb` から読む。システムを書き換えないよう、
   `unshare -m` の中だけで `/usr/bin` と `/usr/share` に overlay で libroot を重ねて起動する。
4. `wsl -- bash -c '...'` は引数が既定のシェルで一度展開される(`$r` が空になる)。スクリプトはファイルにして
   `wsl -d AlmaLinux-9 -- bash /mnt/c/.../x.sh` で動かす(Git Bash からは `MSYS_NO_PATHCONV=1`)。
   WSL のセッションが終わると中の Xvnc も止まるので、背景で動かし続ける。
5. 絵は WSL の python3 から ctypes で libX11 を呼び、窓に XPutImage で貼る(`-ac` で X の接続制限を外す)。

### 実測で分かったこと(2026-10-04)

- Xvnc 相手に Raw・RRE・Hextile・ZRLE・Tight(劣化なし)とも画素単位で一致。Tight + JPEG は PSNR 65dB。
  VNC 認証(vncpasswd -f で作ったパスワード)で、正しいものはつながり、違うものは断られる。
- 自前サーバー相手、1920×1080 の全面が毎フレーム変わる絵: 高画質 109 回/秒(742Mbps)、標準 171、
  劣化なし 31(1168Mbps。zlib の展開が律速。1 回 約 30ms)。
- 同じ条件で TigerVNC 1.16.2 のビューアは 62 回/秒、ふだんの操作のような絵で 64 回/秒(こちらは 534〜606)。
- D3D11 の機能レベル 11.1、ティアリング可。描画の回数は更新の回数より少ない(先に来た更新は 1 回の描画にまとまる)。
- 画面が止まっていれば 10 秒間の CPU 時間は 0ms。専用メモリは 1080p で 82MB、スレッド 56(大半は GPU ドライバ)。
- `tools/inputcheck.py` のマウスは「押す直前の移動」で判定する。本物のマウスカーソルが新しく出た窓に
  重なっていると、その位置への移動が先にサーバーへ届くため(最初の移動で判定していたら NG になった)。
- Python は DPI 非対応なので、ほかの窓の大きさを GetClientRect で聞くと拡大率で割った値が返る
  (クライアントの窓 1920×1080 が 1536×864)。

### メモリが多すぎる、という報告(2026-10-04、v3)

相手が 4K(3840×2160)、`iivnc-server -dryrun` の画面に 12 秒つないだときのクライアント:

| 版 | 専用メモリ | ワーキング セット |
|---|---|---|
| v2(`UpdateSubresource` で送る) | 205〜270MB(測るたびに増減) | 155〜220MB |
| v3 既定(受け渡し用テクスチャ) | 137〜141MB | 88〜91MB |
| v3 `render=gdi` | 41MB | 58MB |

- 原因: `UpdateSubresource` はドライバがその都度、送る分の写しを取って後で GPU へ流す。更新が続くと
  写しが溜まり、全面の更新が重なると 1 枚 33MB ずつ膨らむ。
- 対策: `D3D11_USAGE_STAGING` のテクスチャ(1024×1024)を 2 枚、交互に `Map` して書き、`CopySubresourceRegion`
  で本体へ写す。**`Map` は画面の写しの錠の外で行う**(最初に 4K 1 枚の受け渡し用を錠の中で `Map` したら、
  GPU 待ちで受信側が止まり更新の回数が落ちた)。`IDXGIDevice1::SetMaximumFrameLatency(1)` で
  先行するフレームの分も減らした。高画質 102 回/秒、劣化なし 30 回/秒で、速さは落ちていない。
- 残りの 約 95MB(D3D11 と GDI の差)は GPU ドライバ自体が抱える分で、こちらからは減らせない。

### 描画の既定を GDI に、画面から切り替え(2026-10-04、v4)

利用者の実機で `render=gdi` にすると 30MB 超 → 12.6MB(UltraVNC のビューア 11.0MB とほぼ同じ)だったので、
既定を GDI にし、接続の画面とメニューの「描画」で選べるようにした。

- **flip モデルのスワップチェーンを結びつけた窓は、スワップチェーンを捨てても最後の絵が出たまま**で、
  そのあと GDI で描いても映らない(PrintWindow で撮った絵が GPU の絵のまま。`SWP_FRAMECHANGED` でも戻らない)。
  そこでスワップチェーンは窓に重ねた子窓(`iivnc.Client.Canvas`、`WS_DISABLED`、`HTTRANSPARENT`)に
  結びつけ、GDI へ戻すときは子窓ごと捨てる。親の窓は `WS_CLIPCHILDREN`。
- `python tools/rendercheck.py`: GDI → GPU → GDI → GPU と切り替え、窓を縮めた状態で撮って比べる
  (等倍だと GDI と GPU の絵が画素まで同じで、どちらが描いたか見分けられない)。GDI は最初の絵と差 0.00、
  GPU は 2.74(縮め方の違い)。マウスの行き先は `WindowFromPoint` で親の窓(子窓に吸われない)。
  GPU のまま全画面・窓に戻すで子窓が付いてくる。
- 専用メモリ(相手 1920×1080 の静止画、窓 1282×733): GDI 14MB → GPU 82MB → GDI 48MB → GPU 93〜98MB。
  GPU から戻しても D3D11・ドライバの DLL が残る分(約 30MB)は閉じるまで減らない。

### 全画面から抜けられない、という報告(2026-10-04、v2)

利用者の報告: **MSI(Windows 10)でクライアント、この PC でサーバー**。窓のシステム メニューから全画面にすると、
キーが全部相手へ行き、Ctrl+Alt+Enter でも抜けられなかった。

この PC では再現しなかった(`python tools/fullscreencheck.py [--menu] [--busy] [--injected]`。どれも Ctrl+Alt+Enter で戻る):
起動時から全画面 / システム メニューから / 全面が毎フレーム変わる絵で描画が忙しいとき / 注入したキー
(input-mouser のように。フックは横取りせず窓へ素通りさせる経路)。`-hooktest` は注入したキーもフックで
横取りする検証用の引数。注入は**クライアントの窓が手前にあるときだけ**行う(そうでなければ利用者の窓へ行く)。

v2 で足したこと: 全画面の上端にマウスを当てると帯(窓に戻す・メニュー・最小化・切断)が出る。
`log=1` のとき、Enter を押すたびに Ctrl・Alt の判定をログに書く(`Enter: Ctrl 1(左 1 右 0)、Alt …`)。
原因は MSI で `log=1` にして再現してもらうまで分からない。

### 確かめていないこと

- 2 台の PC の間で実際に使うこと(LAN 越し、相手の IME の切り替わり、Windows キーの横取り)。
- RealVNC・UltraVNC・TightVNC などの Windows のサーバー。
- 等倍のときのスクロール バー、拡大率の違う画面への移動、GPU を失ったときの作り直し。
  (全画面の切り替えは確かめた: `WM_COMMAND` 0x100 で 3840×2160 を覆い、もう一度で元の位置・大きさ・枠に戻る)
