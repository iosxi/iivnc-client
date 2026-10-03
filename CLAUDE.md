# iivnc-client の作業方針

## リリース運用

**手順は共通の `~/.claude/CLAUDE.md`「修正が終わったら、リリースまで通す」に従う。**
ここにはこのリポジトリ固有の事情だけを書く。

- リモート: **まだ無い**(2026-10-04 時点。`git remote -v` が空のうちは、コミットまでで止める)。
  作るなら `iosxi/iivnc-client`(ほかの兄弟と同じ)。
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
- Python は DPI 非対応なので、ほかの窓の大きさを GetClientRect で聞くと拡大率で割った値が返る
  (クライアントの窓 1920×1080 が 1536×864)。

### 確かめていないこと

- 2 台の PC の間で実際に使うこと(LAN 越し、相手の IME の切り替わり、Windows キーの横取り)。
- RealVNC・UltraVNC・TightVNC などの Windows のサーバー。
- 等倍のときのスクロール バー、拡大率の違う画面への移動、GPU を失ったときの作り直し。
  (全画面の切り替えは確かめた: `WM_COMMAND` 0x100 で 3840×2160 を覆い、もう一度で元の位置・大きさ・枠に戻る)
