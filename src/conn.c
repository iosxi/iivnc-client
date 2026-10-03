/* ==================================================================
 * conn.c - 通信のスレッド
 *
 *  接続、RFB の手順(3.3 / 3.7 / 3.8。認証は なし / VNC 認証 /
 *  TightVNC の Tight 認証)、受信と復号、送信。
 *
 *  速さのための工夫
 *   - 更新(FramebufferUpdate)の見出しを受けたら、復号を始める前に
 *     次の要求を出す。サーバーは次の絵の取り込み・符号化を、こちらの
 *     受信・復号と並行して進められる(要求が 1 つ先に出ている)。
 *   - 受信は大きなバッファでまとめて読む。
 *   - カーソルはサーバーから形を受け取り、こちらで描く(動かしても
 *     往復を待たない)。
 *
 *  送信(キー・マウス・クリップボード)は画面のスレッドからも呼ばれるので
 *  g_sendCs で 1 つずつにする。
 * ================================================================== */

#include "iivncc.h"
#include "vncdes.h"
#include <stdarg.h>

Remote g_rm;

static SOCKET           g_sock = INVALID_SOCKET;
static HANDLE           g_thread;
static HWND             g_notify;
static ConnParams       g_p;
static volatile LONG    g_stop;
static CRITICAL_SECTION g_sendCs;
static BOOL             g_inited;
static volatile LONG    g_qemuOk;
static volatile LONG    g_extClip;
static DWORD            g_clipPeer;
static WCHAR            g_err[512];
static BOOL             g_authFailed, g_needPw;
static volatile LONG    g_active;
static volatile LONG    g_quality = -1;

/* 受信バッファ */
static BYTE *g_rb;
static int   g_rbCap, g_rbPos, g_rbLen;

#define CLIP_TEXT     1u
#define CLIP_CAPS     (1u << 24)
#define CLIP_REQUEST  (1u << 25)
#define CLIP_PEEK     (1u << 26)
#define CLIP_NOTIFY   (1u << 27)
#define CLIP_PROVIDE  (1u << 28)
#define CLIP_MAX      (16u << 20)

/* ------------------------------------------------------------------ */
/*  受信                                                                */
/* ------------------------------------------------------------------ */

static BOOL fill(int need)
{
    if (g_rbLen - g_rbPos >= need) return TRUE;
    if (g_rbPos) {
        memmove(g_rb, g_rb + g_rbPos, (size_t)(g_rbLen - g_rbPos));
        g_rbLen -= g_rbPos;
        g_rbPos = 0;
    }
    if (need > g_rbCap) {
        int cap = need + (1 << 20);
        BYTE *p = (BYTE *)realloc(g_rb, (size_t)cap);
        if (!p) return FALSE;
        g_rb = p;
        g_rbCap = cap;
    }
    while (g_rbLen < need) {
        int r = recv(g_sock, (char *)g_rb + g_rbLen, g_rbCap - g_rbLen, 0);
        if (r <= 0 || g_stop) return FALSE;
        g_rbLen += r;
        InterlockedAdd64(&g_rm.bytes, r);
    }
    return TRUE;
}

BOOL rd(void *buf, int n)
{
    if (n <= 0) return TRUE;
    if (!fill(n)) return FALSE;
    memcpy(buf, g_rb + g_rbPos, (size_t)n);
    g_rbPos += n;
    return TRUE;
}

BYTE *rd_ptr(int n)
{
    BYTE *p;
    if (!fill(n)) return NULL;
    p = g_rb + g_rbPos;
    g_rbPos += n;
    return p;
}

BOOL rd_skip(int n)
{
    while (n > 0) {
        int k = n > (1 << 20) ? (1 << 20) : n;
        if (!rd_ptr(k)) return FALSE;
        n -= k;
    }
    return TRUE;
}

/* 大きな矩形のために広げた受信バッファを、使い終わったら縮める */
static void rd_trim(void)
{
    int left = g_rbLen - g_rbPos;
    if (g_rbCap <= (8 << 20) || left > (1 << 20)) return;
    memmove(g_rb, g_rb + g_rbPos, (size_t)left);
    g_rbLen = left;
    g_rbPos = 0;
    {
        BYTE *p = (BYTE *)realloc(g_rb, 2 << 20);
        if (p) { g_rb = p; g_rbCap = 2 << 20; }
    }
}

static BOOL g_rdFail;
unsigned rd_u8(void)  { BYTE b[1]; if (!rd(b, 1)) { g_rdFail = TRUE; return 0; } return b[0]; }
unsigned rd_u16(void) { BYTE b[2]; if (!rd(b, 2)) { g_rdFail = TRUE; return 0; } return ((unsigned)b[0] << 8) | b[1]; }
unsigned rd_u32(void) { BYTE b[4]; if (!rd(b, 4)) { g_rdFail = TRUE; return 0; } return ((unsigned)b[0] << 24) | ((unsigned)b[1] << 16) | ((unsigned)b[2] << 8) | b[3]; }

/* ------------------------------------------------------------------ */
/*  送信                                                                */
/* ------------------------------------------------------------------ */

static BOOL send_all(const void *data, int len)
{
    const char *p = (const char *)data;
    BOOL ok = TRUE;
    EnterCriticalSection(&g_sendCs);
    while (len > 0 && g_sock != INVALID_SOCKET) {
        int r = send(g_sock, p, len, 0);
        if (r <= 0) { ok = FALSE; break; }
        p += r;
        len -= r;
    }
    LeaveCriticalSection(&g_sendCs);
    return ok && len == 0;
}

static void put16(BYTE *p, unsigned v) { p[0] = (BYTE)(v >> 8); p[1] = (BYTE)v; }
static void put32(BYTE *p, unsigned v) { p[0] = (BYTE)(v >> 24); p[1] = (BYTE)(v >> 16); p[2] = (BYTE)(v >> 8); p[3] = (BYTE)v; }

static void send_request(BOOL incremental)
{
    BYTE m[10];
    int w, h;
    AcquireSRWLockShared(&g_rm.lock);
    w = g_rm.w; h = g_rm.h;
    ReleaseSRWLockShared(&g_rm.lock);
    m[0] = 3; m[1] = (BYTE)incremental;
    put16(m + 2, 0); put16(m + 4, 0); put16(m + 6, (unsigned)w); put16(m + 8, (unsigned)h);
    send_all(m, 10);
}

void conn_request_full(void)
{
    if (g_active) send_request(FALSE);
}

static void send_encodings(int q)
{
    int  list[40], n = 0, i;
    BYTE m[4 + 40 * 4];

    if (g_forceEnc >= 0) {      /* 検証用: そのエンコーディングだけ */
        list[n++] = g_forceEnc;
    } else {
        list[n++] = 7;          /* Tight */
        list[n++] = 16;         /* ZRLE */
        list[n++] = 5;          /* Hextile */
        list[n++] = 2;          /* RRE */
    }
    list[n++] = 1;              /* CopyRect */
    list[n++] = 0;              /* Raw */
    list[n++] = -239;           /* カーソル */
    list[n++] = -240;           /* X のカーソル */
    list[n++] = -232;           /* カーソルの位置 */
    list[n++] = -223;           /* 画面の大きさ */
    list[n++] = -308;           /* 画面の大きさ(拡張) */
    list[n++] = -224;           /* LastRect */
    list[n++] = -258;           /* QEMU のキー(スキャン コード) */
    list[n++] = (int)0xC0A1E5CE;/* 拡張クリップボード */
    switch (q) {
    case Q_LOSSLESS:            /* 画質を言わない = JPEG を使わない */
        list[n++] = -256 + 1;
        break;
    case Q_NORMAL:
        list[n++] = -32 + 6; list[n++] = -512 + 80; list[n++] = -767; list[n++] = -256 + 2;
        break;
    case Q_LOW:
        list[n++] = -32 + 2; list[n++] = -512 + 40; list[n++] = -767; list[n++] = -256 + 6;
        break;
    default:                    /* 高画質: 見て分からない程度の JPEG(TurboVNC の既定と同じ考え) */
        list[n++] = -32 + 8; list[n++] = -512 + 95; list[n++] = -768; list[n++] = -256 + 1;
        break;
    }
    m[0] = 2; m[1] = 0; put16(m + 2, (unsigned)n);
    for (i = 0; i < n; i++) put32(m + 4 + i * 4, (unsigned)list[i]);
    send_all(m, 4 + n * 4);
}

void conn_set_quality(int q)
{
    InterlockedExchange(&g_quality, q);
    if (g_active) {
        send_encodings(q);
        send_request(FALSE);
    }
}

void conn_send_pointer(int mask, int x, int y)
{
    BYTE m[6];
    if (!g_active) return;
    m[0] = 5; m[1] = (BYTE)mask;
    put16(m + 2, (unsigned)(x < 0 ? 0 : x)); put16(m + 4, (unsigned)(y < 0 ? 0 : y));
    send_all(m, 6);
}

void conn_send_key(BOOL down, unsigned keysym, unsigned scan)
{
    if (!g_active) return;
    if (g_qemuOk && scan) {
        BYTE m[12];
        m[0] = 255; m[1] = 0;
        put16(m + 2, (unsigned)down);
        put32(m + 4, keysym);
        put32(m + 8, scan);
        send_all(m, 12);
    } else if (keysym) {
        BYTE m[8];
        m[0] = 4; m[1] = (BYTE)down; m[2] = m[3] = 0;
        put32(m + 4, keysym);
        send_all(m, 8);
    }
}

BOOL conn_qemu_keys(void) { return g_qemuOk != 0; }

static void send_ext_clip(unsigned flags, const BYTE *payload, int len)
{
    BYTE *m = (BYTE *)malloc((size_t)len + 12);
    if (!m) return;
    m[0] = 6; m[1] = m[2] = m[3] = 0;
    put32(m + 4, (unsigned)-(len + 4));
    put32(m + 8, flags);
    if (len) memcpy(m + 12, payload, (size_t)len);
    send_all(m, len + 12);
    free(m);
}

static void provide_clipboard(void)
{
    char *cur = NULL;
    int   clen = 0;
    clip_get_current(&cur, &clen);
    if (!cur) return;
    {
        size_t  rawLen = (size_t)clen + 5;
        BYTE   *raw = (BYTE *)malloc(rawLen), *z = (BYTE *)malloc(zd_bound(rawLen) + 16);
        ZDWork *zw = zd_work_new();
        if (raw && z && zw) {
            size_t zl;
            put32(raw, (unsigned)(clen + 1));
            memcpy(raw + 4, cur, (size_t)clen);
            raw[4 + clen] = 0;
            zl = zd_compress(zw, raw, 0, rawLen, z, 1, 1);
            send_ext_clip(CLIP_PROVIDE | CLIP_TEXT, z, (int)zl);
        }
        free(raw); free(z); zd_work_free(zw);
    }
    free(cur);
}

void conn_send_clipboard(const char *utf8, int len)
{
    if (!g_active || g_p.viewOnly) return;
    if (g_extClip) {
        if (!g_clipPeer || (g_clipPeer & CLIP_NOTIFY)) send_ext_clip(CLIP_NOTIFY | CLIP_TEXT, NULL, 0);
        else provide_clipboard();
        return;
    }
    {
        /* 昔からの形: Latin-1、改行は LF */
        WCHAR *w = utf8_to_utf16(utf8, len);
        if (w) {
            int   i, k = 0, n = lstrlenW(w);
            BYTE *m = (BYTE *)malloc((size_t)n + 8);
            if (m) {
                for (i = 0; i < n; i++) {
                    if (w[i] == L'\r') continue;
                    m[8 + k++] = w[i] < 256 ? (BYTE)w[i] : '?';
                }
                m[0] = 6; m[1] = m[2] = m[3] = 0;
                put32(m + 4, (unsigned)k);
                send_all(m, k + 8);
                free(m);
            }
            free(w);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  初期化の手順                                                        */
/* ------------------------------------------------------------------ */

static void set_error(const WCHAR *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(g_err, ARRAYSIZE(g_err) - 1, fmt, ap);
    va_end(ap);
    g_err[ARRAYSIZE(g_err) - 1] = 0;
}

static void read_reason(const WCHAR *prefix)
{
    unsigned n = rd_u32();
    char *s;
    if (g_rdFail || n > 4096) { set_error(L"%s", prefix); return; }
    s = (char *)malloc(n + 1);
    if (!s || !rd(s, (int)n)) { free(s); set_error(L"%s", prefix); return; }
    s[n] = 0;
    {
        WCHAR *w = utf8_to_utf16(s, (int)n);
        set_error(L"%s\n(%s)", prefix, w ? w : L"");
        free(w);
    }
    free(s);
}

static BOOL vnc_auth(void)
{
    BYTE ch[16], resp[16];
    if (!g_p.password[0]) {
        g_needPw = TRUE;
        set_error(L"パスワードが必要です。");
        return FALSE;
    }
    if (!rd(ch, 16)) return FALSE;
    vncdes_response(g_p.password, ch, resp);
    return send_all(resp, 16);
}

static BOOL security_result(int minor)
{
    unsigned r = rd_u32();
    if (g_rdFail) { set_error(L"認証の途中で切れました。"); return FALSE; }
    if (r == 0) return TRUE;
    g_authFailed = TRUE;
    if (minor >= 8) read_reason(L"パスワードが違うか、接続を断られました。");
    else set_error(L"パスワードが違うか、接続を断られました。");
    return FALSE;
}

/* TightVNC の Tight 認証(種類 16)。トンネルは使わず、中の認証を選ぶ */
static BOOL tight_security(BOOL *usedTight)
{
    unsigned n = rd_u32(), i, pick = 0;
    BYTE cap[16];
    if (g_rdFail) return FALSE;
    if (n) {
        BYTE z[4] = { 0 };
        if (!rd_skip((int)n * 16)) return FALSE;
        if (!send_all(z, 4)) return FALSE;      /* トンネルなし */
    }
    n = rd_u32();
    if (g_rdFail) return FALSE;
    *usedTight = TRUE;
    if (!n) return TRUE;                        /* 認証なし */
    for (i = 0; i < n; i++) {
        unsigned code;
        if (!rd(cap, 16)) return FALSE;
        code = ((unsigned)cap[0] << 24) | ((unsigned)cap[1] << 16) | ((unsigned)cap[2] << 8) | cap[3];
        if (code == 2 || (code == 1 && !pick)) pick = code;
    }
    if (!pick) { set_error(L"このサーバーの認証の方式には対応していません(Tight 認証の中)。"); return FALSE; }
    put32(cap, pick);
    if (!send_all(cap, 4)) return FALSE;
    if (pick == 2) return vnc_auth();
    return TRUE;
}

static BOOL handshake(void)
{
    BYTE v[12], b[24];
    int  major, minor, sec = 0;
    BOOL usedTight = FALSE;
    unsigned nameLen;

    if (!rd(v, 12) || memcmp(v, "RFB ", 4)) { set_error(L"VNC サーバーではないようです。"); return FALSE; }
    major = atoi((const char *)v + 4);
    minor = atoi((const char *)v + 8);
    if (major != 3) { set_error(L"知らない RFB の版です(%d.%d)。", major, minor); return FALSE; }
    minor = minor >= 8 ? 8 : minor == 7 ? 7 : 3;
    sprintf((char *)v, "RFB 003.00%d\n", minor);
    if (!send_all(v, 12)) return FALSE;

    if (minor == 3) {
        sec = (int)rd_u32();
        if (g_rdFail) return FALSE;
        if (sec == 0) { read_reason(L"接続を断られました。"); return FALSE; }
    } else {
        BYTE types[256];
        int  n = (int)rd_u8(), i;
        BOOL has1 = FALSE, has2 = FALSE, has16 = FALSE;
        if (g_rdFail) return FALSE;
        if (n == 0) { read_reason(L"接続を断られました。"); return FALSE; }
        if (!rd(types, n)) return FALSE;
        for (i = 0; i < n; i++) {
            if (types[i] == 1) has1 = TRUE;
            if (types[i] == 2) has2 = TRUE;
            if (types[i] == 16) has16 = TRUE;
        }
        if (has2) sec = 2;
        else if (has16) sec = 16;
        else if (has1) sec = 1;
        else {
            WCHAR list[128] = L"";
            for (i = 0; i < n && i < 16; i++) wsprintfW(list + lstrlenW(list), L"%s%d", i ? L", " : L"", types[i]);
            set_error(L"このサーバーの認証の方式(%s)には対応していません。\n"
                      L"サーバー側で「VNC パスワード」の認証を許可してください(暗号化した認証 VeNCrypt / RA2 には未対応)。", list);
            return FALSE;
        }
        b[0] = (BYTE)sec;
        if (!send_all(b, 1)) return FALSE;
    }

    if (sec == 2) {
        if (!vnc_auth() || !security_result(minor)) return FALSE;
    } else if (sec == 16) {
        if (!tight_security(&usedTight) || !security_result(minor)) return FALSE;
    } else if (sec == 1) {
        if (minor >= 8 && !security_result(minor)) return FALSE;
    } else {
        set_error(L"このサーバーの認証の方式(%d)には対応していません。", sec);
        return FALSE;
    }

    b[0] = 1;                                   /* 共有する(ほかの接続を切らない) */
    if (!send_all(b, 1) || !rd(b, 24)) return FALSE;
    AcquireSRWLockExclusive(&g_rm.lock);
    g_rm.w = (b[0] << 8) | b[1];
    g_rm.h = (b[2] << 8) | b[3];
    free(g_rm.fb);
    g_rm.fb = (BYTE *)calloc((size_t)g_rm.w * g_rm.h + 1, 4);
    SetRect(&g_rm.dirty, 0, 0, g_rm.w, g_rm.h);
    ReleaseSRWLockExclusive(&g_rm.lock);
    nameLen = ((unsigned)b[20] << 24) | ((unsigned)b[21] << 16) | ((unsigned)b[22] << 8) | b[23];
    {
        char *name = (char *)malloc(nameLen + 1);
        WCHAR *w;
        if (!name || nameLen > 65536 || !rd(name, (int)nameLen)) { free(name); return FALSE; }
        name[nameLen] = 0;
        w = utf8_to_utf16(name, (int)nameLen);
        if (!w || !w[0] || (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, (int)nameLen, NULL, 0) == 0 && nameLen)) {
            /* UTF-8 でなければ Latin-1 として読む */
            unsigned i;
            free(w);
            w = (WCHAR *)malloc((nameLen + 1) * sizeof(WCHAR));
            if (w) { for (i = 0; i < nameLen; i++) w[i] = (BYTE)name[i]; w[nameLen] = 0; }
        }
        lstrcpynW(g_rm.name, w ? w : L"", ARRAYSIZE(g_rm.name));
        free(w);
        free(name);
    }
    if (usedTight) {
        /* Tight 認証のときは、続けて対応する機能の一覧が来る */
        unsigned ns = rd_u16(), nc = rd_u16(), ne = rd_u16();
        rd_u16();
        if (g_rdFail || !rd_skip((int)(ns + nc + ne) * 16)) return FALSE;
    }
    if (!g_rm.w || !g_rm.h || !g_rm.fb) { set_error(L"相手の画面の大きさが 0 です。"); return FALSE; }

    /* 32bpp 深さ 24 リトルエンディアン R16 G8 B0(BGRX) */
    {
        BYTE m[20] = { 0, 0, 0, 0, 32, 24, 0, 1, 0, 255, 0, 255, 0, 255, 16, 8, 0, 0, 0, 0 };
        if (!send_all(m, 20)) return FALSE;
    }
    send_encodings(g_quality >= 0 ? g_quality : g_p.quality);
    send_request(FALSE);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  受けたメッセージ                                                    */
/* ------------------------------------------------------------------ */

static BOOL server_cut_text(void)
{
    BYTE h[7];
    int  len;
    if (!rd(h, 7)) return FALSE;
    len = (int)(((unsigned)h[3] << 24) | ((unsigned)h[4] << 16) | ((unsigned)h[5] << 8) | h[6]);
    if (len < 0) {
        unsigned n = (unsigned)-len, flags;
        BYTE *p;
        if (n > CLIP_MAX + 64) return FALSE;
        p = rd_ptr((int)n);
        if (!p) return FALSE;
        if (n < 4) return TRUE;
        flags = ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
        if (flags & CLIP_CAPS) {
            BYTE sz[4];
            g_clipPeer = flags;
            InterlockedExchange(&g_extClip, 1);
            put32(sz, CLIP_MAX);
            send_ext_clip(CLIP_CAPS | CLIP_TEXT | CLIP_REQUEST | CLIP_PEEK | CLIP_NOTIFY | CLIP_PROVIDE, sz, 4);
            /* 今のクリップボードを知らせておく */
            if (!g_p.viewOnly) send_ext_clip(CLIP_NOTIFY | CLIP_TEXT, NULL, 0);
        } else if (flags & CLIP_REQUEST) {
            if ((flags & CLIP_TEXT) && !g_p.viewOnly) provide_clipboard();
        } else if (flags & CLIP_PEEK) {
            if (!g_p.viewOnly) send_ext_clip(CLIP_NOTIFY | CLIP_TEXT, NULL, 0);
        } else if (flags & CLIP_NOTIFY) {
            if (flags & CLIP_TEXT) send_ext_clip(CLIP_REQUEST | CLIP_TEXT, NULL, 0);
        } else if (flags & CLIP_PROVIDE) {
            BYTE  *out = NULL;
            size_t olen = zi_inflate_all(p + 4, n - 4, &out, CLIP_MAX + 8);
            if (olen != (size_t)-1 && olen >= 4 && (flags & CLIP_TEXT)) {
                unsigned tlen = ((unsigned)out[0] << 24) | ((unsigned)out[1] << 16) | ((unsigned)out[2] << 8) | out[3];
                if (tlen <= olen - 4) {
                    WCHAR *w;
                    while (tlen && !out[4 + tlen - 1]) tlen--;
                    w = utf8_to_utf16((const char *)out + 4, (int)tlen);
                    if (w) PostMessageW(g_notify, WM_APP_SETCLIP, 0, (LPARAM)w);
                }
            }
            free(out);
        }
        return TRUE;
    }
    if ((unsigned)len > CLIP_MAX) return rd_skip(len);
    {
        BYTE  *p = rd_ptr(len);
        WCHAR *w;
        int    i, k = 0;
        if (!p && len) return FALSE;
        w = (WCHAR *)malloc(((size_t)len * 2 + 1) * sizeof(WCHAR));
        if (w) {
            for (i = 0; i < len; i++) {
                if (p[i] == '\n' && (i == 0 || p[i - 1] != '\r')) w[k++] = L'\r';
                w[k++] = p[i];
            }
            w[k] = 0;
            PostMessageW(g_notify, WM_APP_SETCLIP, 0, (LPARAM)w);
        }
    }
    return TRUE;
}

static BOOL framebuffer_update(void)
{
    BYTE h[3];
    int  n, i, w0, h0;
    BOOL last = FALSE, resized;
    LARGE_INTEGER t0, t1;

    if (!rd(h, 3)) return FALSE;
    n = (h[1] << 8) | h[2];
    /* 次の要求を先に出す(サーバーは次の絵の用意を始められる) */
    send_request(TRUE);
    QueryPerformanceCounter(&t0);
    w0 = g_rm.w; h0 = g_rm.h;
    for (i = 0; (n == 0xFFFF || i < n) && !last; i++) {
        BYTE r[12];
        int  x, y, w, hh, enc;
        if (!rd(r, 12)) return FALSE;
        x = (r[0] << 8) | r[1]; y = (r[2] << 8) | r[3];
        w = (r[4] << 8) | r[5]; hh = (r[6] << 8) | r[7];
        enc = (int)(((unsigned)r[8] << 24) | ((unsigned)r[9] << 16) | ((unsigned)r[10] << 8) | r[11]);
        if (enc == -258) { InterlockedExchange(&g_qemuOk, 1); continue; }
        if (!decode_rect(x, y, w, hh, enc, &last)) {
            if (!g_err[0]) set_error(L"受け取った絵を読めませんでした(エンコーディング %d)。", enc);
            return FALSE;
        }
    }
    decode_finish();
    rd_trim();
    decode_trim();
    QueryPerformanceCounter(&t1);
    InterlockedAdd64(&g_rm.decodeTicks, t1.QuadPart - t0.QuadPart);
    InterlockedIncrement(&g_rm.updates);
    resized = g_rm.w != w0 || g_rm.h != h0;
    if (resized) send_request(FALSE);

    AcquireSRWLockExclusive(&g_rm.lock);
    if (!g_rm.framePosted && !IsRectEmpty(&g_rm.dirty)) {
        g_rm.framePosted = TRUE;
        PostMessageW(g_notify, WM_APP_FRAME, 0, 0);
    } else if (!g_rm.framePosted) {
        /* 何も変わらない更新でも、数え方のために知らせる */
        g_rm.framePosted = TRUE;
        PostMessageW(g_notify, WM_APP_FRAME, 0, 0);
    }
    ReleaseSRWLockExclusive(&g_rm.lock);
    return TRUE;
}

static BOOL message_loop(void)
{
    for (;;) {
        unsigned t = rd_u8();
        if (g_rdFail) return FALSE;
        switch (t) {
        case 0:
            if (!framebuffer_update()) return FALSE;
            break;
        case 1: {                       /* SetColourMapEntries(使わない) */
            BYTE h[5];
            if (!rd(h, 5)) return FALSE;
            if (!rd_skip(((h[3] << 8) | h[4]) * 6)) return FALSE;
            break;
        }
        case 2:
            PostMessageW(g_notify, WM_APP_BELL, 0, 0);
            break;
        case 3:
            if (!server_cut_text()) return FALSE;
            break;
        case 150:                       /* EndOfContinuousUpdates */
            break;
        case 248: {                     /* ServerFence: 求められたら返す */
            BYTE h[8], payload[64], m[73];
            unsigned flags;
            if (!rd(h, 8)) return FALSE;
            if (h[7] > 64 || !rd(payload, h[7])) return FALSE;
            flags = ((unsigned)h[3] << 24) | ((unsigned)h[4] << 16) | ((unsigned)h[5] << 8) | h[6];
            if (flags & 0x80000000u) {
                m[0] = 248; m[1] = m[2] = m[3] = 0;
                put32(m + 4, flags & 7u);
                m[8] = h[7];
                memcpy(m + 9, payload, h[7]);
                send_all(m, 9 + h[7]);
            }
            break;
        }
        default:
            set_error(L"知らないメッセージ(%u)を受け取りました。", t);
            return FALSE;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  接続                                                                */
/* ------------------------------------------------------------------ */

BOOL conn_parse_host(const WCHAR *in, WCHAR *host, int hostCap, int *port)
{
    WCHAR tmp[256], *p;
    lstrcpynW(tmp, in, ARRAYSIZE(tmp));
    /* 前後の空白 */
    for (p = tmp; *p == L' '; p++) ;
    {
        int n = lstrlenW(p);
        while (n && p[n - 1] == L' ') p[--n] = 0;
    }
    if (!*p) return FALSE;
    *port = 5900;
    if (*p == L'[') {                   /* [IPv6]:port */
        WCHAR *e = wcschr(p, L']');
        if (!e) return FALSE;
        *e = 0;
        lstrcpynW(host, p + 1, hostCap);
        if (e[1] == L':' && e[2] == L':') *port = _wtoi(e + 3);
        else if (e[1] == L':') { int d = _wtoi(e + 2); *port = d < 100 ? 5900 + d : d; }
        return *port > 0 && *port < 65536;
    }
    {
        WCHAR *dc = wcsstr(p, L"::"), *c = wcschr(p, L':');
        if (c && wcschr(c + 1, L':') && !dc) {   /* 括弧なしの IPv6 */
            lstrcpynW(host, p, hostCap);
            return TRUE;
        }
        if (dc && !wcschr(dc + 2, L':')) {        /* host::port */
            *dc = 0;
            *port = _wtoi(dc + 2);
        } else if (c && !wcschr(c + 1, L':')) {  /* host:display(100 未満)/ host:port */
            int d = _wtoi(c + 1);
            *c = 0;
            *port = d < 100 ? 5900 + d : d;
        }
    }
    lstrcpynW(host, p, hostCap);
    if (!host[0]) lstrcpyW(host, L"localhost");
    return *port > 0 && *port < 65536;
}

static SOCKET connect_to(const WCHAR *host, int port)
{
    ADDRINFOW hints, *res = NULL, *ai;
    WCHAR     ps[16];
    SOCKET    s = INVALID_SOCKET;
    int       err;
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    wsprintfW(ps, L"%d", port);
    err = GetAddrInfoW(host, ps, &hints, &res);
    if (err) {
        set_error(L"「%s」が見つかりません。名前かアドレスを確かめてください。", host);
        return INVALID_SOCKET;
    }
    for (ai = res; ai && !g_stop; ai = ai->ai_next) {
        u_long nb = 1;
        fd_set ws, es;
        struct timeval tv = { 10, 0 };
        s = socket(ai->ai_family, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) continue;
        ioctlsocket(s, FIONBIO, &nb);
        if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0) goto ok;
        if (WSAGetLastError() == WSAEWOULDBLOCK) {
            FD_ZERO(&ws); FD_SET(s, &ws);
            FD_ZERO(&es); FD_SET(s, &es);
            if (select(0, NULL, &ws, &es, &tv) > 0 && FD_ISSET(s, &ws)) goto ok;
        }
        closesocket(s);
        s = INVALID_SOCKET;
        continue;
    ok:
        nb = 0;
        ioctlsocket(s, FIONBIO, &nb);
        break;
    }
    FreeAddrInfoW(res);
    if (s == INVALID_SOCKET && !g_stop)
        set_error(L"%s のポート %d につながりません。\nサーバーが動いているか、ファイアウォールで止められていないか確かめてください。", host, port);
    return s;
}

static DWORD WINAPI conn_thread(void *arg)
{
    WCHAR *reason = NULL;
    (void)arg;
    g_sock = connect_to(g_p.host, g_p.port);
    if (g_sock != INVALID_SOCKET) {
        int one = 1;
        setsockopt(g_sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
        decode_reset();
        if (handshake()) {
            InterlockedExchange(&g_active, 1);
            log_printf(L"%s:%d に接続した(%dx%d「%s」)", g_p.host, g_p.port, g_rm.w, g_rm.h, g_rm.name);
            PostMessageW(g_notify, WM_APP_CONNECTED, 0, 0);
            message_loop();
            InterlockedExchange(&g_active, 0);
            jpeg_wait_all();
        }
    }
    if (g_err[0] && !g_stop) {
        size_t n = wcslen(g_err) + 1;
        reason = (WCHAR *)malloc(n * sizeof(WCHAR));
        if (reason) memcpy(reason, g_err, n * sizeof(WCHAR));
    }
    log_printf(L"切れた: %s", g_err[0] ? g_err : L"(相手が閉じた)");
    EnterCriticalSection(&g_sendCs);
    if (g_sock != INVALID_SOCKET) closesocket(g_sock);
    g_sock = INVALID_SOCKET;
    LeaveCriticalSection(&g_sendCs);
    if (!g_stop) PostMessageW(g_notify, WM_APP_CLOSED, 0, (LPARAM)reason);
    else free(reason);
    return 0;
}

void conn_start(const ConnParams *p, HWND notify)
{
    if (!g_inited) {
        WSADATA wd;
        WSAStartup(MAKEWORD(2, 2), &wd);
        InitializeCriticalSection(&g_sendCs);
        InitializeSRWLock(&g_rm.lock);
        jpeg_init();
        g_inited = TRUE;
    }
    conn_stop();
    g_p = *p;
    g_notify = notify;
    g_stop = 0;
    g_err[0] = 0;
    g_authFailed = g_needPw = FALSE;
    g_qemuOk = 0;
    g_extClip = 0;
    g_clipPeer = 0;
    g_rbPos = g_rbLen = 0;
    g_rdFail = FALSE;
    g_rm.updates = 0;
    g_rm.bytes = 0;
    g_rm.decodeTicks = 0;
    g_rm.haveCursorEnc = FALSE;
    g_rm.framePosted = FALSE;
    if (g_quality < 0) g_quality = p->quality;
    g_thread = CreateThread(NULL, 0, conn_thread, NULL, 0, NULL);
}

void conn_stop(void)
{
    if (!g_thread) return;
    InterlockedExchange(&g_stop, 1);
    EnterCriticalSection(&g_sendCs);
    if (g_sock != INVALID_SOCKET) shutdown(g_sock, SD_BOTH);
    LeaveCriticalSection(&g_sendCs);
    WaitForSingleObject(g_thread, 5000);
    CloseHandle(g_thread);
    g_thread = NULL;
    InterlockedExchange(&g_active, 0);
}

BOOL conn_active(void) { return g_active != 0; }
const WCHAR *conn_last_error(void) { return g_err; }
BOOL conn_auth_failed(void) { return g_authFailed; }
BOOL conn_needs_password(void) { return g_needPw; }
