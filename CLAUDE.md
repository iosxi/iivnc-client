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
`src/fwrules.c`(ファイアウォールの、この exe の規則を数える・消す)も同じく先頭の `#include` だけ違う。
`src/filexfer.c`(ファイルのコピー＆貼り付け)も同じく先頭の `#include` だけ違う。
ファイルのコピー＆貼り付けの仕組み・検証(`../iivnc-server/tools/fxcheck.py`)・実測は、サーバーの CLAUDE.md に書いた。
クライアントの検証用の引数: `-fxoffer`(つながったら今のクリップボードのファイルを渡す)、`-fxnowatch`(コピーしたファイルを渡さない)。

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

### ほかのビューアとの比べ合いと高速化(2026-10-06、client v10 / server v8)

- `python tools/viewercmp.py [--quality high|lossless] [iivnc-gdi iivnc-gpu ultravnc tightvnc]`。サーバーは
  127.0.0.1:5998・パスワード `bench`・`-testsrc video -testfps 0`。数えるのはサーバーのログの 5 秒ごとの回/秒と、
  ビューアのプロセスの CPU 時間・専用メモリ。**UltraVNC のビューアは、認証の無いサーバーには「信用するか」の確認を
  出して止まる**ので、パスワードを付ける。他社のビューアは画質の段階 8(iivnc-server では JPEG 92・4:4:4)。
- スレッドごとの CPU を見ると(QueryThreadCycleTime と開始アドレス)、高画質ではほぼ全部が JPEG の作業スレッド(WIC)。
  WIC は libjpeg-turbo(PIL)と同じくらい速いので替えていない。
- GPU 描画のとき `nvwgf2umx.dll` のスレッドが 676M サイクル/秒使っていた。受け渡し用のテクスチャを `Map(WRITE)` すると、
  GPU がまだそこから写している間ドライバが CPU を回して待つ(1 回 3〜9M サイクル)。コピーの後にフェンス
  (`ID3D11DeviceContext4::Signal`)を打ち、次に同じテクスチャを使う前に、まだなら `SetEventOnCompletion` で眠って待つ。
  `Map` は 0.1M サイクルに、ドライバのスレッドは 164M に、全体で約 1 割減。更新の回数は落ちなかった(サーバーの
  取り込みでは落ちたので、サーバーには入れていない)。D3D11.4 が無ければ今までどおり。
- 劣化なしでは通信のスレッド 1 本が zlib の展開をする。zinflate.c に速い道を足して 31 → 37 回/秒。その後の律速は
  受信待ち(通信のスレッドは 6 割しか働いていない)。
- `rendercheck.py` の「中心のマウスの行き先」が NG になったのは、`PickerHost.exe` の `Shell_SystemDim`(画面全体を
  覆う窓)が出ていたため。変更前の exe でも同じ NG だった。

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

### システム メニューのチェックが変わらない、という報告(2026-10-04、v5)

利用者の報告: タイトル バーの右クリックのメニューで「画質」「描画」「速さを表示」を変えても、チェックが動かない。

- 原因(ログで実測): タイトル バーの右クリックで届く `WM_INITMENU` の wParam は、`GetSystemMenu(hwnd, FALSE)` とは
  **別のハンドル**(外側の入れ物)。それと比べてからチェックを付け直していたので、一度も付け直していなかった。
  中身のメニューは `WM_INITMENUPOPUP`(HIWORD(lParam) = 1)で届く。→ そこで `update_checks`。
  F8 のメニューは開くたびに作るので、もともと正しかった。
- 画質は**その場で変わっていた**: 選ぶと SetEncodings と全面の(差分でない)要求を送る。サーバーのログで
  JPEG 画質 95 → 40 → 劣化なし がその場で届き、劣化なしでつないで「細い回線」にすると 2 回目の更新(139KB)で
  画面の 31% の画素が置き換わった(残りは単色で、どの画質でも劣化しない部分)。
- `python tools/menucheck.py`: システム メニューを開き(WM_CONTEXTMENU)、開いたメニューの窓から
  `MN_GETHMENU` でハンドルを得てチェックを読む。**クライアントのプロセスのメニューだけを拾う**こと
  (クラス名 `#32768` だけで探すと、利用者の別のアプリのメニューを拾って結果が揺れた)。
  別のプロセスから `GetSystemMenu` を呼ぶ試験は、クライアントが命令を受け付けなくなったので使わない。

### スリープさせない(2026-10-05、server v7 / client v9)

利用者の要望: つないでいる間はスリープさせない。決めたこと: サーバー(接続されている間)とクライアント(つないでいる間)の
両方、画面の消灯も止める、既定オン(server `nosleep=1`、client `nosleep=1`)。

- `PowerCreateRequest`(理由の文字列つき)+ `PowerSetRequest(SystemRequired と DisplayRequired)`。
  サーバーは `WM_APP_CLIENTS`(接続の数が変わった)と設定の変更・分身の読み直しで `power_update()`、
  クライアントはつないだとき(`on_connected`)に止め、切れたとき(`WM_APP_CLOSED`)に解く。プロセスが終われば Windows が解く。
- `python ../iivnc-server/tools/nosleepcheck.py`: 管理者の `powercfg /requests` で、つないでいる間はサーバーと
  クライアントの両方が SYSTEM と DISPLAY に出て、閉じたら消える、nosleep=0 なら出ない、を確かめる(ALL OK)。
  サーバー側から切れたときも、クライアントはすぐ解く(記録で確認)。
- 確かめていないこと: サービスの分身(SYSTEM)からの電源の要求。

### 窓に合わせているのに、最大化で絵が小さい、という報告(2026-10-05、v7)

利用者の報告: MSI(Windows 10)のクライアントから、この PC(4K)のサーバーへ。「窓に合わせる」なのに、
最大化(全画面ではない)すると絵が窓より小さい。(その前の同じような報告は「等倍」だった。)

- この PC(Windows 11)では再現しない: `python tools/maxcheck.py [ini の行...]`(-testsrc の 1920×1080 を拡大)と、
  本物の 4K(-dryrun)を縮める向きのどちらでも、GDI・GPU とも最大化で倍率 0.873 → 0.959、絵は窓いっぱい。
- v7 で `log=1` のとき、大きさが変わるたびに「配置: 窓の中身 WxH、相手 WxH、窓に合わせる、倍率、絵 (l,t)-(r,b)、
  GDI/GPU、最大化」を書く。
- **v8 にしたら MSI でも再現しなくなった**(2026-10-05、利用者の報告)。原因は特定していない(記録を取る前に
  消えた。v7〜v8 で描画まわりは記録を足しただけなので、古い版や古い ini の状態によるものだった可能性がある)。
  また出たら、`log=1` の「配置」の行を見る。

### ネットワークの許可(fwrules.c、2026-10-04)

- Windows が確認の画面で作る規則は、レジストリ上の名前が `TCP Query User{GUID}<exe のパス>` だが、
  **COM(INetFwRule)から見える Name は表示名(FileDescription)** で、TCP と UDP、別の場所の同じ exe の規則とも同じ。
  `INetFwRules::Remove` は名前で消すので、**消す規則だけを一意な名前に付け替えてから消す**
  (仮の規則で実測: 同じ名前の A・B のうち A だけを付け替えて消し、B が残った)。
- 読むのは一般の権限で読めた(COM の Rules を列挙)。消すのは管理者が要るので、自分を `-remove-firewall` で
  管理者として起動し、終了コード(消した数、-1 = 失敗)を受け取る。
- `python ../iivnc-server/tools/fwcheck.py [exe]`: exe を `build/fwtest/` に写し、その写しの仮の規則
  (Windows と同じ表示名、無効にしたもの)を管理者の PowerShell で作り、画面のボタンを押して、写しの規則が 0 件、
  本物の規則の数が変わらないことを見る(利用者の本物の規則には触れない)。
- 2026-10-04 にこの PC のレジストリを `iivnc` で検索: サーバーの確認画面の規則 2 件(パブリックで許可)、
  通知領域のアイコンの記録(`NotifyIconSettings`、`NotifyIconGeneratedAumid_<番号>` の通知設定など)、
  Windows がアプリすべてに付ける記録(MuiCache、FeatureUsage、互換性アシスタント)。サービスの登録・
  `SoftwareSASGeneration` は残っていなかった(サービスをやめたとき戻っている)。

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
