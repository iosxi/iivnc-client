/* ==================================================================
 * iivncc.h - iivnc-client 全体で使う宣言
 *
 *  構成
 *    main.c     起動、引数、接続から表示までの流れ
 *    config.c   iivnc-client.ini の読み書き、ログ
 *    conn.c     通信のスレッド(RFB の手順、受信、要求の先出し、送信)
 *    decode.c   エンコーディングの復号(Raw/CopyRect/RRE/Hextile/ZRLE/Tight)
 *    jpeg.c     JPEG の復号(WIC。作業スレッドで並列に)
 *    view.c     表示の窓(D3D11 / GDI)、拡大縮小、全画面、マウス
 *    keymap.c   キーの変換(仮想キー → キーシム / スキャン コード)
 *    clip.c     クリップボードの受け渡し
 *    ui.c       接続の画面
 *    theme.c    ライト/ダークの配色(kotemado と同じもの)
 *    zdeflate.c zinflate.c vncdes.c  共通部品(iivnc-server と同じもの)
 * ================================================================== */
#ifndef IIVNCC_H
#define IIVNCC_H

#ifndef UNICODE
#error "UNICODE を定義してビルドする(build.bat は /DUNICODE を付けている)"
#endif

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "zlite.h"

#define APP_NAME      L"iivnc-client"
#define APP_VERSION   L"1.1.0"

#define WM_APP_CONNECTED  (WM_APP + 1)  /* 初期化まで済んだ */
#define WM_APP_FRAME      (WM_APP + 2)  /* 更新を 1 回受け終えた */
#define WM_APP_RESIZE     (WM_APP + 3)  /* 相手の画面の大きさが変わった */
#define WM_APP_CURSOR     (WM_APP + 4)  /* カーソルの形が来た */
#define WM_APP_CLOSED     (WM_APP + 5)  /* 切れた。lParam = 理由(malloc した WCHAR*、無ければ NULL) */
#define WM_APP_SETCLIP    (WM_APP + 6)  /* lParam = 相手から来た文字(malloc した WCHAR*) */
#define WM_APP_POINTER    (WM_APP + 7)  /* 相手がカーソルを動かした */
#define WM_APP_BELL       (WM_APP + 8)

/* ------------------------------------------------------------------ */
/*  設定(config.c)                                                     */
/* ------------------------------------------------------------------ */

enum { Q_HIGH, Q_LOSSLESS, Q_NORMAL, Q_LOW, Q_COUNT };
enum { GRAB_FULLSCREEN, GRAB_ALWAYS, GRAB_NEVER };

#define MAX_HISTORY 16

typedef struct Config {
    WCHAR host[256];            /* 今回の接続先 */
    char  password[9];
    BOOL  savePassword;
    int   quality;              /* Q_HIGH など */
    BOOL  viewOnly;
    BOOL  fullscreen;
    BOOL  fit;                  /* 窓に合わせて縮める(FALSE = 等倍) */
    int   grab;                 /* システムのキーを相手へ送るとき */
    BOOL  showStats;            /* タイトルに速さを出す */
    int   theme;
    int   log;
    WCHAR history[MAX_HISTORY][256];
    int   nhistory;
} Config;

extern Config    g_cfg;
extern WCHAR     g_iniPath[MAX_PATH];
extern WCHAR     g_exeDir[MAX_PATH];
extern HINSTANCE g_inst;

/* 検証用(main.c) */
extern WCHAR g_dumpPath[MAX_PATH];  /* -dump: 更新を受けたら絵を BMP に書いて終わる */
extern int   g_exitAfter;           /* -exitafter N: N 回の更新で終わる(0 = 無し) */
extern int   g_idleExitMs;          /* -idleexit ms: 更新が止まってこの時間で終わる */
extern int   g_forceEnc;
extern BOOL  g_hookTest;            /* -hooktest: 注入したキーもフックで横取りする(検証用) */            /* -encoding: そのエンコーディングだけを求める(-1 = 普段どおり) */

void config_init(void);
void config_load(void);
BOOL config_save(void);
void config_add_history(const WCHAR *host);
BOOL config_saved_password(const WCHAR *host, char *out);
void config_set_password(const WCHAR *host, const char *pw);   /* pw が空なら消す */
void log_open(void);
void log_printf(const WCHAR *fmt, ...);
char  *utf16_to_utf8(const WCHAR *s, int *outLen);
WCHAR *utf8_to_utf16(const char *s, int len);

/* ------------------------------------------------------------------ */
/*  相手の画面(conn.c / decode.c)                                      */
/* ------------------------------------------------------------------ */

typedef struct Remote {
    SRWLOCK lock;               /* fb の作り直しと、描画の写しを守る */
    int     w, h;
    BYTE   *fb;                 /* BGRX、1 行 = w*4 */
    WCHAR   name[256];
    /* 前回の描画から変わった範囲(lock で守る) */
    RECT    dirty;
    BOOL    framePosted;
    /* カーソル(lock で守る) */
    int     curW, curH, curHotX, curHotY, curVer;
    BYTE   *curPix;             /* BGRA(A = 0 か 255) */
    BOOL    haveCursorEnc;      /* 相手がカーソルを送ってくる */
    int     ptrX, ptrY;         /* 相手が動かしたカーソルの位置 */
    /* 統計 */
    volatile LONG64 bytes;
    volatile LONG   updates;
    volatile LONG64 decodeTicks;
} Remote;

extern Remote g_rm;

typedef struct ConnParams {
    WCHAR host[256];
    int   port;
    char  password[9];
    int   quality;
    BOOL  viewOnly;
} ConnParams;

BOOL conn_parse_host(const WCHAR *in, WCHAR *host, int hostCap, int *port);
void conn_start(const ConnParams *p, HWND notify);
void conn_stop(void);
BOOL conn_active(void);
void conn_set_quality(int q);
void conn_send_pointer(int mask, int x, int y);
void conn_send_key(BOOL down, unsigned keysym, unsigned scan);   /* scan は QEMU の番号(0 = 無し) */
void conn_send_clipboard(const char *utf8, int len);            /* こちらのクリップボードが変わった */
void conn_request_full(void);
BOOL conn_qemu_keys(void);
const WCHAR *conn_last_error(void);
BOOL conn_auth_failed(void);
BOOL conn_needs_password(void);

/* 受信の読み出し(decode.c が使う) */
BOOL rd(void *buf, int n);
BOOL rd_skip(int n);
BYTE *rd_ptr(int n);            /* 受信の領域を直接見る(次の rd まで有効)。足りなければ NULL */
unsigned rd_u8(void);
unsigned rd_u16(void);
unsigned rd_u32(void);

/* decode.c */
BOOL decode_rect(int x, int y, int w, int h, int enc, BOOL *lastRect);
void decode_reset(void);        /* 接続のたびに(zlib のストリームを作り直す) */
void decode_finish(void);       /* 1 回の更新の終わり(JPEG を待つ) */
void decode_trim(void);         /* 大きな矩形のために広げた作業領域を手放す */
void mark_dirty(int x, int y, int w, int h);

/* jpeg.c */
void jpeg_init(void);
void jpeg_submit(BYTE *data, int len, int x, int y, int w, int h);  /* data は jpeg.c が free する */
void jpeg_wait_all(void);

/* ------------------------------------------------------------------ */
/*  表示(view.c)                                                       */
/* ------------------------------------------------------------------ */

extern HWND g_view;

HWND view_create(void);
void view_set_title(void);
void view_toggle_fullscreen(void);
void view_release_keys(void);

/* keymap.c */
unsigned keymap_keysym(UINT vk, UINT scan, BOOL ext, const BYTE *keyState);
unsigned keymap_qnum(UINT vk, UINT scan, BOOL ext);

/* clip.c */
void clip_init(HWND hwnd);
void clip_on_update(HWND hwnd);
void clip_set_from_remote(HWND hwnd, WCHAR *text);
void clip_get_current(char **utf8, int *len);

/* ui.c */
BOOL ui_connect_dialog(HWND owner, const WCHAR *error);   /* FALSE = やめた */
int  ui_message(HWND owner, const WCHAR *main, const WCHAR *content, int buttons, PCWSTR icon);

/* theme.c */
void     theme_init(void);
BOOL     theme_refresh(void);
BOOL     theme_is_dark(void);
COLORREF theme_back(void);
COLORREF theme_footer(void);
COLORREF theme_ctrl_back(void);
COLORREF theme_text(void);
COLORREF theme_dim_text(void);
COLORREF theme_line(void);
HBRUSH   theme_back_brush(void);
HBRUSH   theme_footer_brush(void);
HBRUSH   theme_ctrl_brush(void);
void     theme_allow_dark(HWND hwnd);
void     theme_apply_dialog(HWND dlg);
LRESULT  theme_ctlcolor(UINT msg, HDC dc, HWND ctl, BOOL dimText);
BOOL     theme_custom_draw_button(NMCUSTOMDRAW *cd, LRESULT *result);

#endif
