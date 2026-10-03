/* ==================================================================
 * jpeg.c - Tight の JPEG 矩形の復号(WIC、作業スレッドで並列に)
 *
 *  JPEG の矩形はほかの矩形に依らないので、受け取った順に作業スレッドへ
 *  渡し、通信のスレッドは次の矩形の読み取りへ進む。復号した画素は fb の
 *  その位置へ直接書く(CopyPixels の行の幅を fb に合わせる)。
 *  1 回の更新の終わり(と CopyRect の前、大きさの変更の前)に待ち合わせる。
 *
 *  WIC の JPEG 展開は 256×256 で 0.15ms ほど(2026-10-03 実測、
 *  1920×1080 で 6.5ms。tools/test_jpeg.py)。
 * ================================================================== */

#include "iivncc.h"
#include <wincodec.h>

typedef struct Job {
    struct Job *next;
    BYTE *data;
    int   len, x, y, w, h;
} Job;

static CRITICAL_SECTION   g_cs;
static CONDITION_VARIABLE g_cvWork, g_cvDone;
static Job               *g_head, *g_tail;
static int                g_pending;
static int                g_nthreads;

static void decode_one(IWICImagingFactory *fac, Job *j)
{
    IWICStream            *st = NULL;
    IWICBitmapDecoder     *dec = NULL;
    IWICBitmapFrameDecode *fr = NULL;
    IWICFormatConverter   *cv = NULL;
    UINT w = 0, h = 0;

    if (FAILED(IWICImagingFactory_CreateStream(fac, &st))) return;
    if (SUCCEEDED(IWICStream_InitializeFromMemory(st, j->data, (DWORD)j->len)) &&
        SUCCEEDED(IWICImagingFactory_CreateDecoderFromStream(fac, (IStream *)st, &GUID_ContainerFormatJpeg,
                                                             WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &fr)) &&
        SUCCEEDED(IWICBitmapFrameDecode_GetSize(fr, &w, &h)) &&
        SUCCEEDED(IWICImagingFactory_CreateFormatConverter(fac, &cv)) &&
        SUCCEEDED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)fr, &GUID_WICPixelFormat32bppBGR,
                                                 WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom))) {
        WICRect rc;
        UINT    stride, size;
        BYTE   *dst;
        AcquireSRWLockShared(&g_rm.lock);
        if (j->x + j->w <= g_rm.w && j->y + j->h <= g_rm.h) {
            rc.X = 0; rc.Y = 0;
            rc.Width = (INT)min(w, (UINT)j->w);
            rc.Height = (INT)min(h, (UINT)j->h);
            stride = (UINT)g_rm.w * 4;
            dst = g_rm.fb + ((size_t)j->y * g_rm.w + j->x) * 4;
            size = stride * (UINT)(rc.Height - 1) + (UINT)rc.Width * 4;
            IWICFormatConverter_CopyPixels(cv, &rc, stride, size, dst);
        }
        ReleaseSRWLockShared(&g_rm.lock);
    } else {
        log_printf(L"JPEG を読めない (%d,%d %dx%d %d バイト)", j->x, j->y, j->w, j->h, j->len);
    }
    if (cv) IWICFormatConverter_Release(cv);
    if (fr) IWICBitmapFrameDecode_Release(fr);
    if (dec) IWICBitmapDecoder_Release(dec);
    if (st) IWICStream_Release(st);
}

static DWORD WINAPI worker(void *arg)
{
    IWICImagingFactory *fac = NULL;
    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&fac);
    for (;;) {
        Job *j;
        EnterCriticalSection(&g_cs);
        while (!g_head) SleepConditionVariableCS(&g_cvWork, &g_cs, INFINITE);
        j = g_head;
        g_head = j->next;
        if (!g_head) g_tail = NULL;
        LeaveCriticalSection(&g_cs);

        if (fac) decode_one(fac, j);
        free(j->data);
        free(j);

        EnterCriticalSection(&g_cs);
        if (--g_pending == 0) WakeAllConditionVariable(&g_cvDone);
        LeaveCriticalSection(&g_cs);
    }
}

void jpeg_init(void)
{
    SYSTEM_INFO si;
    int i;
    InitializeCriticalSection(&g_cs);
    InitializeConditionVariable(&g_cvWork);
    InitializeConditionVariable(&g_cvDone);
    GetSystemInfo(&si);
    g_nthreads = (int)si.dwNumberOfProcessors / 2;
    if (g_nthreads < 2) g_nthreads = 2;
    if (g_nthreads > 8) g_nthreads = 8;
    for (i = 0; i < g_nthreads; i++) {
        HANDLE h = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        if (h) CloseHandle(h);
    }
}

void jpeg_submit(BYTE *data, int len, int x, int y, int w, int h)
{
    Job *j = (Job *)malloc(sizeof(Job));
    if (!j) { free(data); return; }
    j->next = NULL;
    j->data = data; j->len = len;
    j->x = x; j->y = y; j->w = w; j->h = h;
    EnterCriticalSection(&g_cs);
    if (g_tail) g_tail->next = j; else g_head = j;
    g_tail = j;
    g_pending++;
    LeaveCriticalSection(&g_cs);
    WakeConditionVariable(&g_cvWork);
}

void jpeg_wait_all(void)
{
    EnterCriticalSection(&g_cs);
    while (g_pending) SleepConditionVariableCS(&g_cvDone, &g_cs, INFINITE);
    LeaveCriticalSection(&g_cs);
}
