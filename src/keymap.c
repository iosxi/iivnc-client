/* ==================================================================
 * keymap.c - キーの変換
 *
 *  仮想キー → X のキーシム。文字のキーは ToUnicodeEx で今の配列の文字を
 *  求める(Ctrl と Alt は外して聞く。AltGr = Ctrl+Alt のときだけ残す)。
 *  第 6 引数の 4 は「キーボードの状態を変えない」(Windows 10 1607 以降)。
 *  これが無いと、デッド キーの途中状態を壊してしまう。
 *
 *  スキャン コード → QEMU の番号(qnum): E0 付きのキーは 0x80 を立てる。
 *  NumLock は E0 付きで届くが qnum は 0x45、Pause は 0xC6。
 * ================================================================== */

#include "iivncc.h"

typedef struct { UINT vk; unsigned sym, symExt; } VkMap;

/* symExt: E0 付き(拡張キー)のときのキーシム。0 なら sym と同じ */
static const VkMap k_map[] = {
    { VK_BACK, 0xff08, 0 }, { VK_TAB, 0xff09, 0 }, { VK_CLEAR, 0xff9d, 0xff0b }, { VK_RETURN, 0xff0d, 0xff8d },
    { VK_PAUSE, 0xff13, 0 }, { VK_CANCEL, 0xff6b, 0xff6b }, { VK_CAPITAL, 0xffe5, 0 }, { VK_ESCAPE, 0xff1b, 0 },
    { VK_PRIOR, 0xff9a, 0xff55 }, { VK_NEXT, 0xff9b, 0xff56 }, { VK_END, 0xff9c, 0xff57 }, { VK_HOME, 0xff95, 0xff50 },
    { VK_LEFT, 0xff96, 0xff51 }, { VK_UP, 0xff97, 0xff52 }, { VK_RIGHT, 0xff98, 0xff53 }, { VK_DOWN, 0xff99, 0xff54 },
    { VK_SNAPSHOT, 0xff61, 0xff61 }, { VK_INSERT, 0xff9e, 0xff63 }, { VK_DELETE, 0xff9f, 0xffff },
    { VK_LWIN, 0xffeb, 0xffeb }, { VK_RWIN, 0xffec, 0xffec }, { VK_APPS, 0xff67, 0xff67 },
    { VK_NUMPAD0, 0xffb0, 0 }, { VK_NUMPAD1, 0xffb1, 0 }, { VK_NUMPAD2, 0xffb2, 0 }, { VK_NUMPAD3, 0xffb3, 0 },
    { VK_NUMPAD4, 0xffb4, 0 }, { VK_NUMPAD5, 0xffb5, 0 }, { VK_NUMPAD6, 0xffb6, 0 }, { VK_NUMPAD7, 0xffb7, 0 },
    { VK_NUMPAD8, 0xffb8, 0 }, { VK_NUMPAD9, 0xffb9, 0 },
    { VK_MULTIPLY, 0xffaa, 0 }, { VK_ADD, 0xffab, 0 }, { VK_SEPARATOR, 0xffac, 0 }, { VK_SUBTRACT, 0xffad, 0 },
    { VK_DECIMAL, 0xffae, 0 }, { VK_DIVIDE, 0xffaf, 0xffaf },
    { VK_NUMLOCK, 0xff7f, 0xff7f }, { VK_SCROLL, 0xff14, 0 },
    { VK_LSHIFT, 0xffe1, 0 }, { VK_RSHIFT, 0xffe2, 0 }, { VK_LCONTROL, 0xffe3, 0 }, { VK_RCONTROL, 0xffe4, 0xffe4 },
    { VK_LMENU, 0xffe9, 0 }, { VK_RMENU, 0xffea, 0xffea },
    /* 日本語のキー */
    { VK_KANJI, 0xff21, 0 }, { VK_CONVERT, 0xff23, 0 }, { VK_NONCONVERT, 0xff22, 0 }, { VK_KANA, 0xff27, 0 },
    { 0xF3 /* VK_OEM_AUTO */, 0xff2a, 0 }, { 0xF4 /* VK_OEM_ENLW */, 0xff2a, 0 }, { 0xF0 /* VK_OEM_ATTN 英数 */, 0xff30, 0 },
    { 0xF2 /* VK_OEM_COPY カタカナ/ひらがな */, 0xff27, 0 },
    /* メディア・ブラウザ */
    { VK_VOLUME_MUTE, 0x1008ff12, 0x1008ff12 }, { VK_VOLUME_DOWN, 0x1008ff11, 0x1008ff11 }, { VK_VOLUME_UP, 0x1008ff13, 0x1008ff13 },
    { VK_MEDIA_PLAY_PAUSE, 0x1008ff14, 0x1008ff14 }, { VK_MEDIA_STOP, 0x1008ff15, 0x1008ff15 },
    { VK_MEDIA_PREV_TRACK, 0x1008ff16, 0x1008ff16 }, { VK_MEDIA_NEXT_TRACK, 0x1008ff17, 0x1008ff17 },
    { VK_BROWSER_BACK, 0x1008ff26, 0x1008ff26 }, { VK_BROWSER_FORWARD, 0x1008ff27, 0x1008ff27 },
    { VK_BROWSER_REFRESH, 0x1008ff29, 0x1008ff29 }, { VK_BROWSER_SEARCH, 0x1008ff1b, 0x1008ff1b },
    { VK_BROWSER_HOME, 0x1008ff18, 0x1008ff18 }, { VK_LAUNCH_MAIL, 0x1008ff19, 0x1008ff19 },
};

unsigned keymap_keysym(UINT vk, UINT scan, BOOL ext, const BYTE *keyState)
{
    int i;
    BYTE st[256];
    WCHAR buf[8];
    int n;

    /* 左右の区別(Shift はスキャン コードで、Ctrl/Alt は拡張フラグで) */
    if (vk == VK_SHIFT) vk = scan == 0x36 ? VK_RSHIFT : VK_LSHIFT;
    else if (vk == VK_CONTROL) vk = ext ? VK_RCONTROL : VK_LCONTROL;
    else if (vk == VK_MENU) vk = ext ? VK_RMENU : VK_LMENU;
    if (vk >= VK_F1 && vk <= VK_F24) return 0xffbe + (vk - VK_F1);
    for (i = 0; i < (int)ARRAYSIZE(k_map); i++)
        if (k_map[i].vk == vk) return ext && k_map[i].symExt ? k_map[i].symExt : k_map[i].sym;

    /* 文字 */
    memcpy(st, keyState, 256);
    if (!((st[VK_CONTROL] & 0x80) && (st[VK_MENU] & 0x80))) {
        st[VK_CONTROL] = st[VK_LCONTROL] = st[VK_RCONTROL] = 0;
        st[VK_MENU] = st[VK_LMENU] = st[VK_RMENU] = 0;
    }
    n = ToUnicodeEx(vk, scan, st, buf, ARRAYSIZE(buf), 4, GetKeyboardLayout(0));
    if (n < 0) n = 1;                   /* デッド キー: その文字を送る */
    if (n >= 1) {
        unsigned c = buf[0];
        if (n >= 2 && c >= 0xD800 && c < 0xDC00) c = 0x10000 + ((c - 0xD800) << 10) + (buf[1] - 0xDC00);
        if (c < 0x20) return 0;
        if (c < 0x100) return c;
        return 0x01000000 | c;
    }
    /* 文字にならないキーは、Shift なしの文字で */
    n = (int)MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0xFFFF;
    if (n >= 0x20 && n < 0x7F) return (unsigned)(n >= 'A' && n <= 'Z' ? n + 32 : n);
    return 0;
}

unsigned keymap_qnum(UINT vk, UINT scan, BOOL ext)
{
    if (vk == VK_PAUSE) return 0xC6;
    if (vk == VK_NUMLOCK) return 0x45;
    if (!scan || scan > 0x7F) return 0;
    return scan | (ext ? 0x80u : 0);
}
