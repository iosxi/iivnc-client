"""ビューアどうしを、同じ合成の絵(iivnc-server -testsrc)で比べる。

    python tools/viewercmp.py [--src video|static] [--seconds 10] [--quality high] [iivnc-gdi iivnc-gpu ultravnc tightvnc ...]

サーバーは検証用の ini(127.0.0.1:5998、パスワード bench。UltraVNC のビューアは認証が無いと確認を出して止まる)で -testsrc <src> -testfps 0 として動かし、
各ビューアを高画質相当(Tight + JPEG)でつなぐ。数えるのはサーバーのログの 5 秒ごとの集計(回/秒・帯域)と、
ビューアのプロセスの CPU 時間・専用メモリ。ビューアの窓は画面に出る(前面には出さないように頼むが、
他社のビューアは従わないことがある)。
"""
import argparse, ctypes, os, re, socket, subprocess, sys, time
from ctypes import wintypes as W

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVER = os.path.join(ROOT, '..', 'iivnc-server', 'iivnc-server.exe')
TEST = os.path.join(ROOT, 'build', 'test')
SINI = os.path.join(TEST, 'vcmp-server.ini')
SLOG = os.path.join(TEST, 'vcmp-server.log')
CINI = os.path.join(TEST, 'vcmp-client.ini')
PORT = 5998
PASSWORD = 'bench'
Q = ['high']                 # iivnc-client の画質(--quality)。他社のビューアは段階 8(JPEG 92・4:4:4)のまま
UVNC = r'C:\Program Files\uvnc\UltraVNC\vncviewer.exe'
TVNC = r'C:\Program Files\TightVNC\tvnviewer.exe'

VIEWERS = {
    'iivnc-gdi': lambda: [os.path.join(ROOT, 'iivnc-client.exe'), '-ini', CINI, f'127.0.0.1::{PORT}', '-quality', Q[0], '-gdi', '-password', PASSWORD],
    'iivnc-gpu': lambda: [os.path.join(ROOT, 'iivnc-client.exe'), '-ini', CINI, f'127.0.0.1::{PORT}', '-quality', Q[0], '-gpu', '-password', PASSWORD],
    'ultravnc': lambda: [UVNC, f'127.0.0.1::{PORT}', '-encoding', 'tight', '-quality', '8', '-compresslevel', '1',
                         '-autoscaling', '-notoolbar', '-nostatus', '-disablesponsor', '-password', PASSWORD],
    'tightvnc': lambda: [TVNC, '-host=127.0.0.1', f'-port={PORT}', '-encoding=tight', '-jpegimagequality=8',
                         '-compressionlevel=1', '-scale=auto', '-showcontrols=no', f'-password={PASSWORD}'],
}


class PMC(ctypes.Structure):
    _fields_ = [('cb', W.DWORD), ('PageFaultCount', W.DWORD), ('PeakWorkingSetSize', ctypes.c_size_t),
                ('WorkingSetSize', ctypes.c_size_t), ('QuotaPeakPagedPoolUsage', ctypes.c_size_t),
                ('QuotaPagedPoolUsage', ctypes.c_size_t), ('QuotaPeakNonPagedPoolUsage', ctypes.c_size_t),
                ('QuotaNonPagedPoolUsage', ctypes.c_size_t), ('PagefileUsage', ctypes.c_size_t),
                ('PeakPagefileUsage', ctypes.c_size_t), ('PrivateUsage', ctypes.c_size_t)]


def proc_stats(pid):
    k = ctypes.windll.kernel32
    h = k.OpenProcess(0x1000 | 0x0400, False, pid)
    if not h:
        return None, None
    c, e, kt, ut = W.FILETIME(), W.FILETIME(), W.FILETIME(), W.FILETIME()
    k.GetProcessTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(kt), ctypes.byref(ut))
    pmc = PMC(); pmc.cb = ctypes.sizeof(pmc)
    ctypes.windll.psapi.GetProcessMemoryInfo(h, ctypes.byref(pmc), pmc.cb)
    k.CloseHandle(h)
    f = lambda t: (t.dwHighDateTime << 32 | t.dwLowDateTime) / 1e7
    return f(kt) + f(ut), pmc.PrivateUsage / 1048576


def vnc_hide(pw):
    sys.path.insert(0, os.path.join(ROOT, '..', 'iivnc-server', 'tools'))
    import rfbcheck
    key = bytes(int(f'{b:08b}'[::-1], 2) for b in bytes([23, 82, 107, 6, 35, 78, 88, 7]))
    return rfbcheck.des_encrypt(key, pw.encode()[:8].ljust(8, bytes(1))).hex()


def start_server(src):
    os.makedirs(TEST, exist_ok=True)
    if os.path.exists(SLOG):
        os.remove(SLOG)
    with open(SINI, 'w', encoding='utf-8', newline='\n') as f:
        f.write(f'port={PORT}\nlisten=127.0.0.1\nnotify=0\nlog=1\npassword={vnc_hide(PASSWORD)}\n')
    subprocess.Popen([SERVER, '-ini', SINI, '-testsrc', src, '-testfps', '0'])
    for _ in range(50):
        try:
            socket.create_connection(('127.0.0.1', PORT), timeout=0.2).close()
            return
        except OSError:
            time.sleep(0.1)
    raise SystemExit('サーバーが待ち受けない')


def stop_server():
    subprocess.run([SERVER, '-ini', SINI, '-exit'])
    time.sleep(0.5)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', default='video')
    ap.add_argument('--seconds', type=float, default=10)
    ap.add_argument('--quality', default='high', help='iivnc-client の画質(high / lossless / normal / low)')
    ap.add_argument('viewers', nargs='*', default=list(VIEWERS))
    a = ap.parse_args()
    Q[0] = a.quality
    with open(CINI, 'w', encoding='utf-8', newline='\n') as f:
        f.write('[client]\nquality=high\nfit=1\nstats=0\n[general]\nlog=0\n')
    for name in a.viewers:
        start_server(a.src)
        si = subprocess.STARTUPINFO()
        si.dwFlags = 1          # STARTF_USESHOWWINDOW
        si.wShowWindow = 4      # SW_SHOWNOACTIVATE
        p = subprocess.Popen(VIEWERS[name](), startupinfo=si)
        time.sleep(3)
        c0, _ = proc_stats(p.pid)
        t0 = time.perf_counter()
        time.sleep(a.seconds)
        c1, mem = proc_stats(p.pid)
        dt = time.perf_counter() - t0
        p.kill()
        time.sleep(0.5)
        stop_server()
        log = open(SLOG, encoding='utf-8').read()
        rates = [float(m) for m in re.findall(r'([\d.]+) 回/秒', log)][1:]       # 最初の 5 秒は立ち上がりを含むので捨てる
        mbps = [float(m) for m in re.findall(r'([\d.]+) Mbps', log)][1:]
        enc = re.search(r'エンコーディング (.*)', log)
        rate = sum(rates) / len(rates) if rates else float('nan')
        bw = sum(mbps) / len(mbps) if mbps else float('nan')
        cpu = (c1 - c0) / dt * 100 if c0 is not None and c1 is not None else float('nan')
        per = cpu * 10 / rate if rate == rate and rate > 0 else float('nan')
        print(f'{name:10s} {a.src:6s} {rate:6.1f} 回/秒 {bw:7.1f} Mbps  ビューアの CPU {cpu:6.1f}%'
              f' (1 回 {per:5.2f}ms)  専用メモリ {mem:6.1f}MB  | {enc.group(1).strip() if enc else "?"}')


if __name__ == '__main__':
    main()
