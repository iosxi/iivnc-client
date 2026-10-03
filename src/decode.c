/* ==================================================================
 * decode.c - エンコーディングの復号
 *
 *  こちらは常に 32bpp 深さ 24 リトルエンディアン(BGRX)を求めているので、
 *  画素は 4 バイトの BGRX、ZRLE の CPIXEL は 3 バイト(B,G,R)、
 *  Tight の TPIXEL は 3 バイト(R,G,B)。
 *
 *  受信を待つ間に fb の鍵を持たないよう、矩形(Hextile はタイル)の
 *  データを読み切ってから鍵を取って書く。JPEG は jpeg.c の作業スレッドが
 *  fb へ直接書く(更新の終わりに待ち合わせる)。
 * ================================================================== */

#include "iivncc.h"

static ZInflate *g_zrle, *g_tz[4];
static BYTE     *g_tmp;
static size_t    g_tmpCap;
static BYTE     *g_zout;
static size_t    g_zoutCap;

void decode_reset(void)
{
    int i;
    if (!g_zrle) g_zrle = zi_new(); else zi_reset(g_zrle);
    for (i = 0; i < 4; i++) { if (!g_tz[i]) g_tz[i] = zi_new(); else zi_reset(g_tz[i]); }
}

void mark_dirty(int x, int y, int w, int h)
{
    RECT r;
    SetRect(&r, x, y, x + w, y + h);
    AcquireSRWLockExclusive(&g_rm.lock);
    UnionRect(&g_rm.dirty, &g_rm.dirty, &r);
    ReleaseSRWLockExclusive(&g_rm.lock);
}

void decode_finish(void)
{
    jpeg_wait_all();
}

/* 大きな矩形のために広げた作業領域を手放す */
void decode_trim(void)
{
    if (g_tmpCap > (8u << 20)) { free(g_tmp); g_tmp = NULL; g_tmpCap = 0; }
    if (g_zoutCap > (8u << 20)) { free(g_zout); g_zout = NULL; g_zoutCap = 0; }
}

static BOOL in_fb(int x, int y, int w, int h)
{
    return x >= 0 && y >= 0 && w >= 0 && h >= 0 && x + w <= g_rm.w && y + h <= g_rm.h;
}

static __forceinline DWORD *px(int x, int y) { return (DWORD *)g_rm.fb + (size_t)y * g_rm.w + x; }

static void fill(int x, int y, int w, int h, DWORD c)
{
    int i, j;
    for (j = 0; j < h; j++) {
        DWORD *d = px(x, y + j);
        for (i = 0; i < w; i++) d[i] = c;
    }
}

static BYTE *tmp(size_t n)
{
    if (n > g_tmpCap) {
        free(g_tmp);
        g_tmpCap = n + 65536;
        g_tmp = (BYTE *)malloc(g_tmpCap);
        if (!g_tmp) g_tmpCap = 0;
    }
    return g_tmp;
}

/* ------------------------------------------------------------------ */
/*  Raw / CopyRect / RRE / Hextile                                      */
/* ------------------------------------------------------------------ */

static BOOL dec_raw(int x, int y, int w, int h)
{
    BYTE *s = rd_ptr(w * h * 4);
    int j;
    if (!s && w * h) return FALSE;
    AcquireSRWLockExclusive(&g_rm.lock);
    for (j = 0; j < h; j++) memcpy(px(x, y + j), s + (size_t)j * w * 4, (size_t)w * 4);
    ReleaseSRWLockExclusive(&g_rm.lock);
    return TRUE;
}

static BOOL dec_copy(int x, int y, int w, int h)
{
    int sx = (int)rd_u16(), sy = (int)rd_u16(), j;
    if (!in_fb(sx, sy, w, h)) return FALSE;
    /* JPEG がまだ書いている所から写すかもしれない */
    jpeg_wait_all();
    AcquireSRWLockExclusive(&g_rm.lock);
    if (y > sy) for (j = h - 1; j >= 0; j--) memmove(px(x, y + j), px(sx, sy + j), (size_t)w * 4);
    else for (j = 0; j < h; j++) memmove(px(x, y + j), px(sx, sy + j), (size_t)w * 4);
    ReleaseSRWLockExclusive(&g_rm.lock);
    return TRUE;
}

static DWORD pix4(const BYTE *p) { return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16); }

static BOOL dec_rre(int x, int y, int w, int h)
{
    unsigned n = rd_u32(), i;
    BYTE  bg[4], *s;
    if (!rd(bg, 4) || n > 1000000) return FALSE;
    s = rd_ptr((int)n * 12);
    if (!s && n) return FALSE;
    AcquireSRWLockExclusive(&g_rm.lock);
    fill(x, y, w, h, pix4(bg));
    for (i = 0; i < n; i++) {
        const BYTE *r = s + i * 12;
        int rx = (r[4] << 8) | r[5], ry = (r[6] << 8) | r[7], rw = (r[8] << 8) | r[9], rh = (r[10] << 8) | r[11];
        if (rx + rw <= w && ry + rh <= h) fill(x + rx, y + ry, rw, rh, pix4(r));
    }
    ReleaseSRWLockExclusive(&g_rm.lock);
    return TRUE;
}

static BOOL dec_hextile(int x, int y, int w, int h)
{
    DWORD bg = 0, fg = 0;
    int tx, ty;
    for (ty = y; ty < y + h; ty += 16)
        for (tx = x; tx < x + w; tx += 16) {
            int tw = min(16, x + w - tx), th = min(16, y + h - ty), j;
            unsigned sub = rd_u8();
            if (sub & 1) {
                BYTE *s = rd_ptr(tw * th * 4);
                if (!s) return FALSE;
                AcquireSRWLockExclusive(&g_rm.lock);
                for (j = 0; j < th; j++) memcpy(px(tx, ty + j), s + j * tw * 4, (size_t)tw * 4);
                ReleaseSRWLockExclusive(&g_rm.lock);
                continue;
            }
            if (sub & 2) { BYTE b[4]; if (!rd(b, 4)) return FALSE; bg = pix4(b); }
            if (sub & 4) { BYTE b[4]; if (!rd(b, 4)) return FALSE; fg = pix4(b); }
            {
                int n = 0, k, sz;
                BYTE *s = NULL;
                if (sub & 8) {
                    n = (int)rd_u8();
                    sz = (sub & 16) ? 6 : 2;
                    s = rd_ptr(n * sz);
                    if (!s && n) return FALSE;
                }
                AcquireSRWLockExclusive(&g_rm.lock);
                fill(tx, ty, tw, th, bg);
                for (k = 0; k < n; k++) {
                    const BYTE *r = s + k * ((sub & 16) ? 6 : 2);
                    DWORD c = fg;
                    int sx, sy, sw, sh;
                    if (sub & 16) { c = pix4(r); r += 4; }
                    sx = r[0] >> 4; sy = r[0] & 15; sw = (r[1] >> 4) + 1; sh = (r[1] & 15) + 1;
                    if (sx + sw <= tw && sy + sh <= th) fill(tx + sx, ty + sy, sw, sh, c);
                }
                ReleaseSRWLockExclusive(&g_rm.lock);
            }
        }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  ZRLE                                                                */
/* ------------------------------------------------------------------ */

static __forceinline DWORD cpix(const BYTE *p) { return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16); }

static BOOL dec_zrle(int x, int y, int w, int h)
{
    unsigned len = rd_u32();
    BYTE    *src;
    size_t   n, p = 0;
    int      tx, ty;
    BOOL     ok = TRUE;

    if (len > (256u << 20)) return FALSE;
    src = rd_ptr((int)len);
    if (!src && len) return FALSE;
    n = zi_inflate_avail(g_zrle, src, len, &g_zout, &g_zoutCap);
    if (n == (size_t)-1) return FALSE;

#define NEED(k) do { if (p + (size_t)(k) > n) { ok = FALSE; goto done; } } while (0)
    AcquireSRWLockExclusive(&g_rm.lock);
    for (ty = y; ty < y + h; ty += 64)
        for (tx = x; tx < x + w; tx += 64) {
            int tw = min(64, x + w - tx), th = min(64, y + h - ty), i, j;
            unsigned sub;
            const BYTE *d = g_zout;
            NEED(1);
            sub = d[p++];
            if (sub == 0) {
                NEED(tw * th * 3);
                for (j = 0; j < th; j++) {
                    DWORD *o = px(tx, ty + j);
                    for (i = 0; i < tw; i++, p += 3) o[i] = cpix(d + p);
                }
            } else if (sub == 1) {
                NEED(3);
                fill(tx, ty, tw, th, cpix(d + p));
                p += 3;
            } else if (sub <= 16) {
                DWORD pal[16];
                int bits = sub == 2 ? 1 : sub <= 4 ? 2 : 4, rb = (tw * bits + 7) / 8;
                NEED(sub * 3);
                for (i = 0; i < (int)sub; i++, p += 3) pal[i] = cpix(d + p);
                NEED(rb * th);
                for (j = 0; j < th; j++) {
                    DWORD *o = px(tx, ty + j);
                    const BYTE *r = d + p + j * rb;
                    for (i = 0; i < tw; i++) {
                        int bitpos = i * bits, v = (r[bitpos >> 3] >> (8 - bits - (bitpos & 7))) & ((1 << bits) - 1);
                        o[i] = pal[v < (int)sub ? v : 0];
                    }
                }
                p += (size_t)rb * th;
            } else if (sub == 128 || sub >= 130) {
                DWORD pal[128];
                int   np = sub >= 130 ? (int)sub - 128 : 0, k = 0, total = tw * th;
                if (np) {
                    NEED(np * 3);
                    for (i = 0; i < np; i++, p += 3) pal[i] = cpix(d + p);
                }
                while (k < total) {
                    DWORD c;
                    int   run = 1;
                    if (!np) {
                        NEED(3);
                        c = cpix(d + p);
                        p += 3;
                        for (;;) { unsigned b; NEED(1); b = d[p++]; run += (int)b; if (b != 255) break; }
                    } else {
                        unsigned idx;
                        NEED(1);
                        idx = d[p++];
                        if (idx & 128) for (;;) { unsigned b; NEED(1); b = d[p++]; run += (int)b; if (b != 255) break; }
                        c = pal[(idx & 127) < (unsigned)np ? (idx & 127) : 0];
                    }
                    if (run > total - k) run = total - k;
                    while (run-- > 0) {
                        *px(tx + k % tw, ty + k / tw) = c;
                        k++;
                    }
                }
            } else {
                ok = FALSE;
                goto done;
            }
        }
done:
    ReleaseSRWLockExclusive(&g_rm.lock);
#undef NEED
    return ok;
}

/* ------------------------------------------------------------------ */
/*  Tight                                                               */
/* ------------------------------------------------------------------ */

static int compact_len(void)
{
    unsigned b = rd_u8(), n = b & 0x7F;
    if (b & 0x80) {
        b = rd_u8();
        n |= (b & 0x7F) << 7;
        if (b & 0x80) n |= rd_u8() << 14;
    }
    return (int)n;
}

static const BYTE *tight_data(int stream, int size)
{
    BYTE *src, *out;
    int   len;
    if (size < 12) return rd_ptr(size);
    len = compact_len();
    src = rd_ptr(len);
    out = tmp((size_t)size);
    if ((!src && len) || !out) return NULL;
    if (zi_inflate(g_tz[stream], src, (size_t)len, out, (size_t)size)) return NULL;
    return out;
}

static BOOL dec_tight(int x, int y, int w, int h)
{
    unsigned ctl = rd_u8(), kind;
    int i, j;

    for (i = 0; i < 4; i++) if (ctl & (1u << i)) zi_reset(g_tz[i]);
    kind = ctl >> 4;
    if (kind == 8) {                            /* 塗りつぶし */
        BYTE c[3];
        if (!rd(c, 3)) return FALSE;
        AcquireSRWLockExclusive(&g_rm.lock);
        fill(x, y, w, h, (DWORD)c[2] | ((DWORD)c[1] << 8) | ((DWORD)c[0] << 16));
        ReleaseSRWLockExclusive(&g_rm.lock);
        return TRUE;
    }
    if (kind == 9) {                            /* JPEG */
        int   len = compact_len();
        BYTE *s = rd_ptr(len), *copy;
        if (!s || len <= 0) return FALSE;
        copy = (BYTE *)malloc((size_t)len);
        if (!copy) return FALSE;
        memcpy(copy, s, (size_t)len);
        jpeg_submit(copy, len, x, y, w, h);
        return TRUE;
    }
    if (kind > 9) return FALSE;                 /* PNG(TightPNG)は求めていない */
    {
        int stream = (int)(kind & 3), filter = (kind & 4) ? (int)rd_u8() : 0;
        const BYTE *d;
        if (filter == 1) {
            DWORD pal[256];
            int   n = (int)rd_u8() + 1;
            BYTE  pb[768];
            if (!rd(pb, n * 3)) return FALSE;
            for (i = 0; i < n; i++) pal[i] = (DWORD)pb[i * 3 + 2] | ((DWORD)pb[i * 3 + 1] << 8) | ((DWORD)pb[i * 3] << 16);
            if (n == 2) {
                int rb = (w + 7) / 8;
                d = tight_data(stream, rb * h);
                if (!d) return FALSE;
                AcquireSRWLockExclusive(&g_rm.lock);
                for (j = 0; j < h; j++) {
                    DWORD *o = px(x, y + j);
                    const BYTE *r = d + j * rb;
                    for (i = 0; i < w; i++) o[i] = pal[(r[i >> 3] >> (7 - (i & 7))) & 1];
                }
                ReleaseSRWLockExclusive(&g_rm.lock);
            } else {
                d = tight_data(stream, w * h);
                if (!d) return FALSE;
                AcquireSRWLockExclusive(&g_rm.lock);
                for (j = 0; j < h; j++) {
                    DWORD *o = px(x, y + j);
                    const BYTE *r = d + j * w;
                    for (i = 0; i < w; i++) o[i] = pal[r[i] < n ? r[i] : 0];
                }
                ReleaseSRWLockExclusive(&g_rm.lock);
            }
            return TRUE;
        }
        if (filter != 0 && filter != 2) return FALSE;
        d = tight_data(stream, w * h * 3);
        if (!d) return FALSE;
        AcquireSRWLockExclusive(&g_rm.lock);
        if (filter == 0) {
            for (j = 0; j < h; j++) {
                DWORD *o = px(x, y + j);
                const BYTE *r = d + (size_t)j * w * 3;
                for (i = 0; i < w; i++, r += 3) o[i] = (DWORD)r[2] | ((DWORD)r[1] << 8) | ((DWORD)r[0] << 16);
            }
        } else {
            /* 勾配の濾過: 予測 = 左 + 上 - 左上(0..255 に収める)との差 */
            BYTE *prev = (BYTE *)calloc((size_t)w * 3 + 3, 1), *cur = (BYTE *)calloc((size_t)w * 3 + 3, 1);
            if (prev && cur) {
                for (j = 0; j < h; j++) {
                    DWORD *o = px(x, y + j);
                    for (i = 0; i < w; i++) {
                        int c;
                        for (c = 0; c < 3; c++) {
                            int left = i ? cur[(i - 1) * 3 + c] : 0, up = prev[i * 3 + c], ul = i ? prev[(i - 1) * 3 + c] : 0;
                            int pr = left + up - ul;
                            pr = pr < 0 ? 0 : pr > 255 ? 255 : pr;
                            cur[i * 3 + c] = (BYTE)(d[((size_t)j * w + i) * 3 + c] + pr);
                        }
                        o[i] = (DWORD)cur[i * 3 + 2] | ((DWORD)cur[i * 3 + 1] << 8) | ((DWORD)cur[i * 3] << 16);
                    }
                    { BYTE *t = prev; prev = cur; cur = t; }
                }
            }
            free(prev);
            free(cur);
        }
        ReleaseSRWLockExclusive(&g_rm.lock);
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  疑似エンコーディング                                                */
/* ------------------------------------------------------------------ */

static void set_cursor(int w, int h, int hx, int hy, BYTE *bgra)
{
    AcquireSRWLockExclusive(&g_rm.lock);
    free(g_rm.curPix);
    g_rm.curPix = bgra;
    g_rm.curW = w; g_rm.curH = h;
    g_rm.curHotX = hx; g_rm.curHotY = hy;
    g_rm.curVer++;
    g_rm.haveCursorEnc = TRUE;
    ReleaseSRWLockExclusive(&g_rm.lock);
    PostMessageW(g_view, WM_APP_CURSOR, 0, 0);
}

static BOOL dec_cursor(int hx, int hy, int w, int h, BOOL xcursor)
{
    int   mb = (w + 7) / 8, i, j;
    BYTE *bgra, *pixels = NULL, *mask = NULL, fgbg[6];
    if (!w || !h) { set_cursor(0, 0, 0, 0, NULL); return TRUE; }
    if (w > 512 || h > 512) return FALSE;
    if (xcursor && !rd(fgbg, 6)) return FALSE;
    pixels = (BYTE *)malloc(xcursor ? (size_t)mb * h : (size_t)w * h * 4);
    mask = (BYTE *)malloc((size_t)mb * h);
    bgra = (BYTE *)calloc((size_t)w * h, 4);
    if (!pixels || !mask || !bgra || !rd(pixels, xcursor ? mb * h : w * h * 4) || !rd(mask, mb * h)) {
        free(pixels); free(mask); free(bgra);
        return FALSE;
    }
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            BYTE *o = bgra + ((size_t)j * w + i) * 4;
            if (xcursor) {
                const BYTE *c = (pixels[j * mb + i / 8] >> (7 - (i & 7))) & 1 ? fgbg : fgbg + 3;
                o[0] = c[2]; o[1] = c[1]; o[2] = c[0];
            } else {
                const BYTE *s = pixels + ((size_t)j * w + i) * 4;
                o[0] = s[0]; o[1] = s[1]; o[2] = s[2];
            }
            o[3] = (mask[j * mb + i / 8] >> (7 - (i & 7))) & 1 ? 255 : 0;
        }
    free(pixels);
    free(mask);
    set_cursor(w, h, hx, hy, bgra);
    return TRUE;
}

static BOOL resize(int w, int h)
{
    BYTE *fb;
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return FALSE;
    jpeg_wait_all();
    fb = (BYTE *)calloc((size_t)w * h + 1, 4);
    if (!fb) return FALSE;
    AcquireSRWLockExclusive(&g_rm.lock);
    free(g_rm.fb);
    g_rm.fb = fb;
    g_rm.w = w;
    g_rm.h = h;
    SetRect(&g_rm.dirty, 0, 0, w, h);
    ReleaseSRWLockExclusive(&g_rm.lock);
    PostMessageW(g_view, WM_APP_RESIZE, 0, 0);
    log_printf(L"相手の画面の大きさ: %dx%d", w, h);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  入口                                                                */
/* ------------------------------------------------------------------ */

BOOL decode_rect(int x, int y, int w, int h, int enc, BOOL *lastRect)
{
    BOOL ok;
    switch (enc) {
    case -239: return dec_cursor(x, y, w, h, FALSE);
    case -240: return dec_cursor(x, y, w, h, TRUE);
    case -232:
        g_rm.ptrX = x;
        g_rm.ptrY = y;
        PostMessageW(g_view, WM_APP_POINTER, 0, 0);
        return TRUE;
    case -223:
        return resize(w, h);
    case -308: {
        int n = (int)rd_u8();
        if (!rd_skip(3 + n * 16)) return FALSE;
        if (y != 0) return TRUE;        /* こちらの頼み(大きさの変更)への返事で、失敗 */
        return resize(w, h);
    }
    case -224:
        *lastRect = TRUE;
        return TRUE;
    }
    if (!in_fb(x, y, w, h)) {
        log_printf(L"範囲の外の矩形 (%d,%d %dx%d) エンコーディング %d", x, y, w, h, enc);
        return FALSE;
    }
    switch (enc) {
    case 0:  ok = dec_raw(x, y, w, h); break;
    case 1:  ok = dec_copy(x, y, w, h); break;
    case 2:  ok = dec_rre(x, y, w, h); break;
    case 5:  ok = dec_hextile(x, y, w, h); break;
    case 7:  ok = dec_tight(x, y, w, h); break;
    case 16: ok = dec_zrle(x, y, w, h); break;
    default:
        log_printf(L"知らないエンコーディング %d", enc);
        return FALSE;
    }
    if (ok) mark_dirty(x, y, w, h);
    return ok;
}
