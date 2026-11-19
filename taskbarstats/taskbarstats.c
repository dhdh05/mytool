/*
 * TaskbarStats - hien thi CPU / GPU / RAM / VRAM / nhiet do CPU, GPU, SSD tren taskbar Windows.
 * C thuan + Win32 API, khong dung thu vien ngoai, khong can driver.
 *
 * Nguon du lieu:
 *   CPU %      : GetSystemTimes
 *   RAM        : GlobalMemoryStatusEx
 *   GPU %      : PDH  \GPU Engine(*engtype_3D)\Utilization Percentage   (giong Task Manager)
 *   VRAM       : PDH  \GPU Adapter Memory(*)\Dedicated Usage  + DXGI (tong VRAM)
 *   GPU temp   : D3DKMTQueryAdapterInfo(ADAPTERPERFDATA) (WDDM 2.4+, cung nguon voi Task Manager),
 *                fallback NVML (nvml.dll) neu la card NVIDIA
 *   CPU temp   : PDH  \Thermal Zone Information(*)\High Precision Temperature (ACPI thermal zone)
 *   SSD temp   : IOCTL_STORAGE_QUERY_PROPERTY / StorageDeviceTemperatureProperty (o chua Windows)
 *   Pin BT     : SetupAPI, thuoc tinh PnP {104EA319-6EE2-4701-BD47-8DDBF425BBE5} 2
 *                (cung nguon voi muc Bluetooth trong Settings) - chuot, phim, tai nghe Bluetooth
 *
 * GPU / VRAM: mac dinh TAT (de nhe may), bat trong menu chuot phai. Counter GPU chi duoc
 * tao khi co it nhat 1 muc GPU/VRAM dang hien, tat het thi giai phong.
 *
 * Thao tac: keo chuot trai de doi vi tri, chuot phai de mo menu.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <winioctl.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <dxgi.h>
#include <setupapi.h>

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "setupapi.lib")
#endif

#define APPNAME   L"TaskbarStats"
#define RUNKEY    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define CFGKEY    L"Software\\TaskbarStats"
#define IDM_AUTOSTART 1
#define IDM_RESETPOS  2
#define IDM_EXIT      3
#define NA (-1)
#define IDM_ITEM0     10              /* IDM_ITEM0 + IT_xxx: bat/tat tung muc hien thi */

/* Cac muc co the hien thi (thu tu = thu tu tren taskbar, xep theo cot 2 dong) */
enum {
    IT_CPU,
    IT_RAM,
    IT_GPU,
    IT_VRAM,
    IT_SSD,
    IT_BAT,
    IT_COUNT
};
static const WCHAR *itemMenu[IT_COUNT] = {
    // L"CPU (% + nhi\u1ec7t \u0111\u1ed9)",   /* may khong doc duoc nhiet do CPU */
    L"CPU",
    L"RAM",
    L"GPU (% + nhi\u1ec7t \u0111\u1ed9)",
    L"VRAM",
    L"Nhi\u1ec7t \u0111\u1ed9 SSD",
    L"Pin Bluetooth",
};
#define GPU_MASK ((1u << IT_GPU) | (1u << IT_VRAM))
static DWORD showMask = ((1u << IT_COUNT) - 1) & ~GPU_MASK;   /* mac dinh: tat ca tru GPU/VRAM */

/* ------------------------------------------------------------------ */
/* D3DKMT (tu khai bao de khong can WDK)                               */
/* ------------------------------------------------------------------ */
typedef UINT KMT_HANDLE;
typedef struct { LUID AdapterLuid; KMT_HANDLE hAdapter; } KMT_OPENADAPTERFROMLUID;
typedef struct { KMT_HANDLE hAdapter; UINT Type; void *pPrivateDriverData; UINT PrivateDriverDataSize; } KMT_QUERYADAPTERINFO;
typedef struct {
    UINT32    PhysicalAdapterIndex;
    ULONGLONG MemoryFrequency;
    ULONGLONG MaxMemoryFrequency;
    ULONGLONG MaxMemoryFrequencyOC;
    ULONGLONG MemoryBandwidth;
    ULONGLONG PCIEBandwidth;
    ULONG     FanRPM;
    ULONG     Power;        /* 1/10 % */
    ULONG     Temperature;  /* 1/10 do C */
    UCHAR     PowerStateOverride;
} KMT_ADAPTER_PERFDATA;
#define KMTQAITYPE_ADAPTERPERFDATA 62
typedef LONG (APIENTRY *PFN_OpenAdapterFromLuid)(KMT_OPENADAPTERFROMLUID *);
typedef LONG (APIENTRY *PFN_QueryAdapterInfo)(KMT_QUERYADAPTERINFO *);

/* NVML (nap dong) */
typedef int (*PFN_nvmlInit)(void);
typedef int (*PFN_nvmlGetHandle)(unsigned, void **);
typedef int (*PFN_nvmlGetTemp)(void *, int, unsigned *);

/* ------------------------------------------------------------------ */
/* Trang thai                                                          */
/* ------------------------------------------------------------------ */
static struct {
    int cpu, ram, gpu, vram;          /* % */
    ULONGLONG ramUsed, vramUsed;      /* byte */
    int cpuT, gpuT, ssdT;             /* do C, NA = khong doc duoc */
    int bat;                          /* % pin thiet bi Bluetooth, NA = khong co */
} S = { 0, 0, 0, 0, 0, 0, NA, NA, NA, NA };

/* Thuoc tinh PnP Windows dung de luu % pin thiet bi Bluetooth */
static const DEVPROPKEY PKEY_BtBattery =
    { { 0x104EA319, 0x6EE2, 0x4701, { 0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5 } }, 2 };
/* DEVPKEY_Device_FriendlyName va DEVPKEY_Device_DeviceDesc (tu khai bao, khoi can initguid) */
static const DEVPROPKEY PKEY_FriendlyName =
    { { 0xA45C254E, 0xDF1C, 0x4EFD, { 0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0 } }, 14 };
static const DEVPROPKEY PKEY_DeviceDesc =
    { { 0xA45C254E, 0xDF1C, 0x4EFD, { 0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0 } }, 2 };
static WCHAR batName[64] = L"Pin";    /* ten thiet bi co pin thap nhat */

static ULONGLONG pIdle, pKern, pUser;
static PDH_HQUERY   hq;
static PDH_HQUERY   hqGpu;               /* query rieng cho GPU, chi mo khi can */
static PDH_HCOUNTER cGpu, cVram, cTz;
static BOOL tzHighPrec;

static BOOL      gpuFound;
static LUID      gpuLuid;
static UINT      gpuVendor;
static ULONGLONG vramTotal;
static WCHAR     luidStr[48];

static PFN_QueryAdapterInfo pQueryAdapterInfo;
static KMT_HANDLE hKmtAdapter;
static PFN_nvmlGetTemp nvmlGetTemp;
static void *nvmlDev;

static HANDLE hDisk = INVALID_HANDLE_VALUE;
static int    tickCount;

/* Giao dien */
static HWND    hwnd;
static HFONT   fLabel, fValue;
static HDC     memdc;
static HBITMAP dib;
static DWORD  *bits;
static int     dibW, dibH;
static int     dpi = 96;
static int     winX, winY, winW, winH;
static int     lineH, v1W, v2W, gapV, gapL, colGap, pad;
static BOOL    light;
static int     offsetLeft = -1;       /* khoang cach tu mep trai taskbar, -1 = mac dinh (sat goc trai) */
static BOOL    dragging;
static int     dragStartCursor, dragStartWin;
static COLORREF col[4];               /* 0 nhan, 1 binh thuong, 2 canh bao, 3 nguy hiem */

#define SC(v) MulDiv((v), dpi, 96)

/* ------------------------------------------------------------------ */
/* Cam bien                                                            */
/* ------------------------------------------------------------------ */
static ULONGLONG ft2u(FILETIME f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }

static void pick_gpu(void)
{
    IDXGIFactory1 *f = NULL;
    IDXGIAdapter1 *a;
    UINT i;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f))) return;
    for (i = 0; IDXGIFactory1_EnumAdapters1(f, i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(a, &d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            /* uu tien card co VRAM rieng lon nhat (card roi tren laptop 2 GPU) */
            if (!gpuFound || d.DedicatedVideoMemory > vramTotal) {
                gpuFound  = TRUE;
                gpuLuid   = d.AdapterLuid;
                gpuVendor = d.VendorId;
                vramTotal = d.DedicatedVideoMemory;
            }
        }
        IDXGIAdapter1_Release(a);
    }
    IDXGIFactory1_Release(f);
    if (gpuFound)
        wsprintfW(luidStr, L"luid_0x%08X_0x%08X", (UINT)gpuLuid.HighPart, (UINT)gpuLuid.LowPart);
}

static void init_gpu_temp(void)
{
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    PFN_OpenAdapterFromLuid pOpen;
    if (!gpuFound || !gdi) return;
    pOpen             = (PFN_OpenAdapterFromLuid)(void *)GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid");
    pQueryAdapterInfo = (PFN_QueryAdapterInfo)(void *)GetProcAddress(gdi, "D3DKMTQueryAdapterInfo");
    if (pOpen && pQueryAdapterInfo) {
        KMT_OPENADAPTERFROMLUID o;
        o.AdapterLuid = gpuLuid;
        o.hAdapter = 0;
        if (pOpen(&o) == 0) hKmtAdapter = o.hAdapter;
    }
    if (gpuVendor == 0x10DE) { /* NVIDIA: chuan bi NVML lam phuong an du phong */
        HMODULE nv = LoadLibraryW(L"nvml.dll");
        if (!nv) nv = LoadLibraryW(L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
        if (nv) {
            PFN_nvmlInit      init = (PFN_nvmlInit)(void *)GetProcAddress(nv, "nvmlInit_v2");
            PFN_nvmlGetHandle gh   = (PFN_nvmlGetHandle)(void *)GetProcAddress(nv, "nvmlDeviceGetHandleByIndex_v2");
            PFN_nvmlGetTemp   gt   = (PFN_nvmlGetTemp)(void *)GetProcAddress(nv, "nvmlDeviceGetTemperature");
            if (init && gh && gt && init() == 0 && gh(0, &nvmlDev) == 0) nvmlGetTemp = gt;
        }
    }
}

static int read_gpu_temp(void)
{
    if (hKmtAdapter) {
        KMT_ADAPTER_PERFDATA pd;
        KMT_QUERYADAPTERINFO q;
        ZeroMemory(&pd, sizeof pd);
        q.hAdapter = hKmtAdapter;
        q.Type = KMTQAITYPE_ADAPTERPERFDATA;
        q.pPrivateDriverData = &pd;
        q.PrivateDriverDataSize = sizeof pd;
        if (pQueryAdapterInfo(&q) == 0 && pd.Temperature > 0 && pd.Temperature < 1500)
            return (int)((pd.Temperature + 5) / 10);
    }
    if (nvmlGetTemp) {
        unsigned t = 0;
        if (nvmlGetTemp(nvmlDev, 0 /* NVML_TEMPERATURE_GPU */, &t) == 0 && t > 0 && t < 150) return (int)t;
    }
    return NA;
}

static void init_disk(void)
{
    WCHAR win[MAX_PATH], path[32];
    HANDLE v;
    STORAGE_DEVICE_NUMBER sdn;
    DWORD br;
    if (!GetSystemWindowsDirectoryW(win, MAX_PATH)) return;
    wsprintfW(path, L"\\\\.\\%c:", win[0]);
    /* quyen truy cap = 0: chi gui IOCTL truy van, khong can admin */
    v = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (v == INVALID_HANDLE_VALUE) return;
    if (DeviceIoControl(v, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &sdn, sizeof sdn, &br, NULL)) {
        wsprintfW(path, L"\\\\.\\PhysicalDrive%u", (UINT)sdn.DeviceNumber);
        hDisk = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    }
    CloseHandle(v);
}

static int read_ssd_temp(void)
{
    STORAGE_PROPERTY_QUERY q;
    union { STORAGE_TEMPERATURE_DATA_DESCRIPTOR d; BYTE raw[512]; } buf;
    DWORD br = 0;
    if (hDisk == INVALID_HANDLE_VALUE) return NA;
    ZeroMemory(&q, sizeof q);
    q.PropertyId = StorageDeviceTemperatureProperty;
    q.QueryType  = PropertyStandardQuery;
    if (!DeviceIoControl(hDisk, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, &buf, sizeof buf, &br, NULL))
        return NA;
    if (buf.d.InfoCount == 0) return NA;
    {
        int t = buf.d.TemperatureInfo[0].Temperature; /* index 0 = Composite tren NVMe */
        return (t > 0 && t < 120) ? t : NA;
    }
}

/* Quet cac thiet bi dang co mat, lay % pin thap nhat trong cac thiet bi Bluetooth bao pin */
static int read_bt_battery(void)
{
    HDEVINFO h = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    SP_DEVINFO_DATA d;
    DWORD i;
    int best = NA;
    if (h == INVALID_HANDLE_VALUE) return NA;
    d.cbSize = sizeof d;
    for (i = 0; SetupDiEnumDeviceInfo(h, i, &d); i++) {
        DEVPROPTYPE t = 0;
        BYTE v = 0;
        DWORD sz = 0;
        if (SetupDiGetDevicePropertyW(h, &d, &PKEY_BtBattery, &t, &v, sizeof v, &sz, 0) &&
            t == DEVPROP_TYPE_BYTE && v <= 100) {
            if (best == NA || v < best) {
                WCHAR nm[64];
                best = v;
                /* lay ten thiet bi: FriendlyName, khong co thi dung DeviceDesc */
                if ((SetupDiGetDevicePropertyW(h, &d, &PKEY_FriendlyName, &t, (BYTE *)nm, sizeof nm, &sz, 0) ||
                     SetupDiGetDevicePropertyW(h, &d, &PKEY_DeviceDesc, &t, (BYTE *)nm, sizeof nm, &sz, 0)) &&
                    t == DEVPROP_TYPE_STRING && nm[0]) {
                    nm[63] = 0;
                    lstrcpyW(batName, nm);
                }
            }
        }
    }
    SetupDiDestroyDeviceInfoList(h);
    if (best == NA) lstrcpyW(batName, L"Pin");
    return best;
}

static void init_pdh(void)
{
    if (PdhOpenQueryW(NULL, 0, &hq) != ERROR_SUCCESS) { hq = NULL; return; }
    /* PdhAddEnglishCounter: chay dung ca tren Windows tieng Viet / ngon ngu khac */
    if (PdhAddEnglishCounterW(hq, L"\\Thermal Zone Information(*)\\High Precision Temperature", 0, &cTz) == ERROR_SUCCESS)
        tzHighPrec = TRUE;
    else if (PdhAddEnglishCounterW(hq, L"\\Thermal Zone Information(*)\\Temperature", 0, &cTz) != ERROR_SUCCESS)
        cTz = NULL;
    PdhCollectQueryData(hq);
}

/* Counter GPU Engine kha ton CPU -> chi mo query GPU khi user bat muc GPU/VRAM */
static void gpu_query_update(void)
{
    BOOL need = (showMask & GPU_MASK) != 0;
    if (need && !hqGpu) {
        if (PdhOpenQueryW(NULL, 0, &hqGpu) != ERROR_SUCCESS) { hqGpu = NULL; return; }
        if (PdhAddEnglishCounterW(hqGpu, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &cGpu) != ERROR_SUCCESS) cGpu = NULL;
        if (PdhAddEnglishCounterW(hqGpu, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &cVram) != ERROR_SUCCESS) cVram = NULL;
        PdhCollectQueryData(hqGpu);
    } else if (!need && hqGpu) {
        PdhCloseQuery(hqGpu);
        hqGpu = NULL; cGpu = NULL; cVram = NULL;
    }
}

/* Lay mang gia tri cua counter co wildcard. Buffer dung chung: xu ly xong moi goi tiep. */
static PDH_FMT_COUNTERVALUE_ITEM_W *pdh_array(PDH_HCOUNTER c, DWORD *count)
{
    static BYTE *buf;
    static DWORD cap;
    DWORD size = 0;
    *count = 0;
    if (!c) return NULL;
    if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, count, NULL) != (PDH_STATUS)PDH_MORE_DATA)
        return NULL;
    if (size > cap) {
        if (buf) HeapFree(GetProcessHeap(), 0, buf);
        cap = size * 2;
        buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, cap);
        if (!buf) { cap = 0; return NULL; }
    }
    size = cap;
    if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, count,
                                     (PDH_FMT_COUNTERVALUE_ITEM_W *)buf) != ERROR_SUCCESS) {
        *count = 0;
        return NULL;
    }
    return (PDH_FMT_COUNTERVALUE_ITEM_W *)buf;
}

static BOOL valid(const PDH_FMT_COUNTERVALUE_ITEM_W *it)
{
    return it->FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || it->FmtValue.CStatus == PDH_CSTATUS_NEW_DATA;
}

static BOOL mine(const WCHAR *instance)
{
    return !gpuFound || (instance && wcsstr(instance, luidStr));
}

static void sample(void)
{
    FILETIME fi, fk, fu;
    MEMORYSTATUSEX ms;
    DWORD n, i;
    PDH_FMT_COUNTERVALUE_ITEM_W *a;

    /* CPU */
    if (GetSystemTimes(&fi, &fk, &fu)) {
        ULONGLONG I = ft2u(fi), K = ft2u(fk), U = ft2u(fu);
        ULONGLONG di = I - pIdle, dt = (K - pKern) + (U - pUser); /* kernel time da bao gom idle */
        if (dt) S.cpu = (int)((dt - di) * 100 / dt);
        pIdle = I; pKern = K; pUser = U;
    }

    /* RAM */
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) {
        S.ramUsed = ms.ullTotalPhys - ms.ullAvailPhys;
        S.ram = (int)ms.dwMemoryLoad;
    }

    if (hq && PdhCollectQueryData(hq) == ERROR_SUCCESS) {
        /* CPU temp: lay thermal zone nong nhat */
        {
            double best = -1000;
            a = pdh_array(cTz, &n);
            for (i = 0; i < n; i++) {
                if (!valid(&a[i])) continue;
                {
                    double c = tzHighPrec ? a[i].FmtValue.doubleValue / 10.0 - 273.15
                                          : a[i].FmtValue.doubleValue - 273.15;
                    if (c > 1 && c < 150 && c > best) best = c;
                }
            }
            S.cpuT = best > 0 ? (int)(best + 0.5) : NA;
        }
    }

    if (hqGpu && PdhCollectQueryData(hqGpu) == ERROR_SUCCESS) {
        /* GPU %: cong tat ca engine 3D cua GPU da chon */
        double g = 0;
        a = pdh_array(cGpu, &n);
        for (i = 0; i < n; i++)
            if (valid(&a[i]) && mine(a[i].szName)) g += a[i].FmtValue.doubleValue;
        S.gpu = g > 100 ? 100 : (int)(g + 0.5);

        /* VRAM */
        {
            double v = 0;
            a = pdh_array(cVram, &n);
            for (i = 0; i < n; i++)
                if (valid(&a[i]) && mine(a[i].szName)) v += a[i].FmtValue.doubleValue;
            S.vramUsed = (ULONGLONG)v;
            S.vram = vramTotal ? (int)(S.vramUsed * 100 / vramTotal) : 0;
            if (S.vram > 100) S.vram = 100;
        }
    }

    if (showMask & (1u << IT_GPU)) S.gpuT = read_gpu_temp();
    if (tickCount % 5 == 0) S.ssdT = read_ssd_temp(); /* SSD doi cham, 5 giay doc 1 lan */
    if (tickCount % 30 == 0) S.bat = read_bt_battery(); /* pin doi rat cham, 30 giay doc 1 lan */
    tickCount++;
}

/* ------------------------------------------------------------------ */
/* Cau hinh                                                            */
/* ------------------------------------------------------------------ */
static void load_theme(void)
{
    DWORD v = 0, sz = sizeof v;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"SystemUsesLightTheme", RRF_RT_REG_DWORD, NULL, &v, &sz);
    light = v != 0;
    if (light) {
        col[0] = RGB(80, 80, 80);   col[1] = RGB(0, 0, 0);
        col[2] = RGB(170, 100, 0);  col[3] = RGB(200, 0, 0);
    } else {
        col[0] = RGB(170, 170, 170); col[1] = RGB(255, 255, 255);
        col[2] = RGB(255, 200, 60);  col[3] = RGB(255, 90, 90);
    }
}

static void load_offset(void)
{
    DWORD v, sz = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, CFGKEY, L"OffsetLeft", RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS)
        offsetLeft = (int)v;
}

static void load_show(void)
{
    DWORD v, sz = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, CFGKEY, L"Items", RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS &&
        (v & ((1u << IT_COUNT) - 1)))
        showMask = v & ((1u << IT_COUNT) - 1);
}

static void save_show(void)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, CFGKEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return;
    RegSetValueExW(k, L"Items", 0, REG_DWORD, (BYTE *)&showMask, sizeof showMask);
    RegCloseKey(k);
}

static void save_offset(void)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, CFGKEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return;
    if (offsetLeft < 0) RegDeleteValueW(k, L"OffsetLeft");
    else { DWORD v = (DWORD)offsetLeft; RegSetValueExW(k, L"OffsetLeft", 0, REG_DWORD, (BYTE *)&v, sizeof v); }
    RegCloseKey(k);
}

static BOOL autostart_get(void)
{
    HKEY k;
    BOOL r = FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUNKEY, 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        r = RegQueryValueExW(k, APPNAME, NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
        RegCloseKey(k);
    }
    return r;
}

static void autostart_set(BOOL on)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUNKEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        WCHAR p[MAX_PATH], q[MAX_PATH + 4];
        GetModuleFileNameW(NULL, p, MAX_PATH);
        wsprintfW(q, L"\"%s\"", p);
        RegSetValueExW(k, APPNAME, 0, REG_SZ, (BYTE *)q, (DWORD)(lstrlenW(q) + 1) * sizeof(WCHAR));
    } else {
        RegDeleteValueW(k, APPNAME);
    }
    RegCloseKey(k);
}

/* ------------------------------------------------------------------ */
/* Ve                                                                  */
/* ------------------------------------------------------------------ */
static int text_w(HFONT f, const WCHAR *s)
{
    SIZE z;
    HGDIOBJ old = SelectObject(memdc, f);
    GetTextExtentPoint32W(memdc, s, lstrlenW(s), &z);
    SelectObject(memdc, old);
    return z.cx;
}

static int maxi(int a, int b) { return a > b ? a : b; }

static void init_layout(void)
{
    HDC sdc = GetDC(NULL);
    TEXTMETRICW tm;
    HGDIOBJ old;
    dpi = GetDeviceCaps(sdc, LOGPIXELSY);
    ReleaseDC(NULL, sdc);
    if (fLabel) DeleteObject(fLabel);
    if (fValue) DeleteObject(fValue);
    /* ANTIALIASED_QUALITY (khong ClearType) de tach alpha duoc */
    fLabel = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                         ANTIALIASED_QUALITY, 0, L"Segoe UI");
    fValue = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                         ANTIALIASED_QUALITY, 0, L"Segoe UI");
    old = SelectObject(memdc, fValue);
    GetTextMetricsW(memdc, &tm);
    SelectObject(memdc, old);
    lineH  = tm.tmHeight;
    gapV   = SC(5);
    colGap = SC(14);
    pad    = SC(4);
    gapL   = SC(6);
    v1W    = maxi(text_w(fValue, L"100%"), text_w(fValue, L"100\u00B0C"));
    v2W    = maxi(text_w(fValue, L"100\u00B0C"), text_w(fValue, L"99.9G"));
}

/* Chuyen chu trang (GDI) trong rect thanh mau co alpha (premultiplied) */
static void colorize(RECT r, COLORREF c)
{
    int x, y;
    BYTE R = GetRValue(c), G = GetGValue(c), B = GetBValue(c);
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > dibW) r.right = dibW;
    if (r.bottom > dibH) r.bottom = dibH;
    for (y = r.top; y < r.bottom; y++)
        for (x = r.left; x < r.right; x++) {
            DWORD *p = &bits[y * dibW + x];
            DWORD a = *p & 0xFF; /* do phu cua chu trang */
            *p = (a << 24) | ((R * a / 255) << 16) | ((G * a / 255) << 8) | (B * a / 255);
        }
}

static void put(int x, int y, int w, HFONT f, const WCHAR *s, COLORREF c, UINT align)
{
    RECT r;
    r.left = x; r.top = y; r.right = x + w; r.bottom = y + lineH;
    SelectObject(memdc, f);
    DrawTextW(memdc, s, -1, &r, align | DT_SINGLELINE | DT_NOPREFIX | DT_TOP);
    GdiFlush();
    r.left -= 1; r.right += 1;
    colorize(r, c);
}

static int sev(int v, int warn, int crit) { return v == NA ? 1 : v >= crit ? 3 : v >= warn ? 2 : 1; }

/* Pin: cang thap cang nguy hiem */
static int sev_bat(int v) { return v == NA ? 1 : v <= 15 ? 3 : v <= 30 ? 2 : 1; }

static void fmt_pct(WCHAR *b, int v) { wsprintfW(b, L"%d%%", v); }
static void fmt_temp(WCHAR *b, int t) { if (t == NA) lstrcpyW(b, L"--"); else wsprintfW(b, L"%d\u00B0C", t); }
static void fmt_gb(WCHAR *b, ULONGLONG bytes)
{
    ULONGLONG t = (bytes * 10 + (1ULL << 29)) >> 30; /* phan muoi GB */
    if (t >= 1000) wsprintfW(b, L"%uG", (UINT)(t / 10));
    else wsprintfW(b, L"%u.%uG", (UINT)(t / 10), (UINT)(t % 10));
}

/* ------------------------------------------------------------------ */
/* Bo cuc: moi muc la 1 "o" (nhan + 1-2 gia tri), xep thanh cot 2 dong */
/* ------------------------------------------------------------------ */
typedef struct {
    const WCHAR *lab;
    WCHAR v1[16], v2[16];
    int s1, s2;
    BOOL has2;
} Cell;

static Cell cells[IT_COUNT];
static int  nCells;
static int  colX[IT_COUNT], colLW[IT_COUNT], colHas2[IT_COUNT]; /* theo cot */

static void make_cell(int it, Cell *c)
{
    c->has2 = TRUE;
    c->s2 = 1;
    switch (it) {
    case IT_CPU:
        c->lab = L"CPU";
        fmt_pct(c->v1, S.cpu);  c->s1 = sev(S.cpu, 70, 90);
        fmt_temp(c->v2, S.cpuT); c->s2 = sev(S.cpuT, 75, 90);
        if (S.cpuT == NA) c->has2 = FALSE;   /* khong doc duoc nhiet do -> chi hien % */
        break;
    case IT_RAM:
        c->lab = L"RAM";
        fmt_pct(c->v1, S.ram);  c->s1 = sev(S.ram, 75, 90);
        fmt_gb(c->v2, S.ramUsed);
        break;
    case IT_GPU:
        c->lab = L"GPU";
        fmt_pct(c->v1, S.gpu);  c->s1 = sev(S.gpu, 70, 90);
        fmt_temp(c->v2, S.gpuT); c->s2 = sev(S.gpuT, 75, 87);
        if (S.gpuT == NA) c->has2 = FALSE;   /* driver khong bao nhiet do -> chi hien % */
        break;
    case IT_VRAM:
        c->lab = L"VRAM";
        fmt_pct(c->v1, S.vram); c->s1 = sev(S.vram, 75, 90);
        fmt_gb(c->v2, S.vramUsed);
        break;
    case IT_SSD:
        c->lab = L"SSD";
        fmt_temp(c->v1, S.ssdT); c->s1 = sev(S.ssdT, 60, 70);
        c->has2 = FALSE;
        break;
    case IT_BAT:
        c->lab = batName;        /* ten thiet bi Bluetooth co pin thap nhat */
        if (S.bat == NA) lstrcpyW(c->v1, L"--"); else fmt_pct(c->v1, S.bat);
        c->s1 = sev_bat(S.bat);
        c->has2 = FALSE;
        break;
    }
}

/* Tinh noi dung cac o dang bat va do rong cua so (winW) */
static void layout(void)
{
    int it, j, x, nCols;
    nCells = 0;
    for (it = 0; it < IT_COUNT; it++)
        if (showMask & (1u << it)) make_cell(it, &cells[nCells++]);

    nCols = (nCells + 1) / 2;
    x = pad;
    for (j = 0; j < nCols; j++) {
        int r, lw = 0, w;
        colHas2[j] = FALSE;
        for (r = 0; r < 2 && 2 * j + r < nCells; r++) {
            Cell *c = &cells[2 * j + r];
            lw = maxi(lw, text_w(fLabel, c->lab));
            if (c->has2) colHas2[j] = TRUE;
        }
        if (lw > SC(110)) lw = SC(110);           /* ten dai qua thi cat "..." */
        colLW[j] = lw;
        colX[j] = x;
        w = lw + gapL + v1W + (colHas2[j] ? gapV + v2W : 0);
        x += w + colGap;
    }
    winW = (nCols ? x - colGap : x) + pad;
}

static void render(void)
{
    int j, r, i, nCols = (nCells + 1) / 2;

    if (!dib || dibW != winW || dibH != winH) {
        BITMAPINFO bi;
        if (dib) DeleteObject(dib);
        ZeroMemory(&bi, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = winW;
        bi.bmiHeader.biHeight = -winH; /* top-down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        dib = CreateDIBSection(memdc, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
        SelectObject(memdc, dib);
        dibW = winW; dibH = winH;
    }
    ZeroMemory(bits, (SIZE_T)dibW * dibH * 4);
    SetBkMode(memdc, TRANSPARENT);
    SetTextColor(memdc, RGB(255, 255, 255));

    for (j = 0; j < nCols; j++) {
        int rows = (2 * j + 1 < nCells) ? 2 : 1;
        int y = (winH - rows * lineH) / 2;        /* cot chi co 1 o thi can giua */
        for (r = 0; r < rows; r++, y += lineH) {
            Cell *c = &cells[2 * j + r];
            int x = colX[j];
            put(x, y, colLW[j], fLabel, c->lab, col[0], DT_LEFT | DT_END_ELLIPSIS);
            x += colLW[j] + gapL;
            put(x, y, v1W, fValue, c->v1, col[c->s1], DT_RIGHT);
            if (c->has2) put(x + v1W + gapV, y, v2W, fValue, c->v2, col[c->s2], DT_RIGHT);
        }
    }

    /* alpha = 1 cho nen: gan nhu trong suot nhung van bat duoc chuot */
    for (i = 0; i < dibW * dibH; i++)
        if (!(bits[i] >> 24)) bits[i] = 0x01000000;
}

static BOOL fullscreen_app(void)
{
    HWND fg = GetForegroundWindow();
    WCHAR cls[64];
    RECT r;
    MONITORINFO mi;
    if (!fg || fg == GetDesktopWindow() || fg == GetShellWindow() || fg == hwnd) return FALSE;
    GetClassNameW(fg, cls, 64);
    if (!lstrcmpW(cls, L"WorkerW") || !lstrcmpW(cls, L"Progman") || !lstrcmpW(cls, L"Shell_TrayWnd")) return FALSE;
    GetWindowRect(fg, &r);
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi);
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
           r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
}

/* Tinh vi tri, ve lai va day cua so len tren taskbar */
static void update_window(void)
{
    HWND tb = FindWindowW(L"Shell_TrayWnd", NULL);
    RECT t;
    MONITORINFO mi;
    POINT dst, src = { 0, 0 };
    SIZE sz;
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };

    if (!tb || !GetWindowRect(tb, &t)) { ShowWindow(hwnd, SW_HIDE); return; }
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromWindow(tb, MONITOR_DEFAULTTOPRIMARY), &mi);

    /* An khi: taskbar doc, taskbar dang tu an, hoac co app full screen */
    if ((t.bottom - t.top) > (t.right - t.left) ||
        t.top >= mi.rcMonitor.bottom - SC(4) || t.bottom <= mi.rcMonitor.top + SC(4) ||
        fullscreen_app()) {
        ShowWindow(hwnd, SW_HIDE);
        return;
    }

    winH = t.bottom - t.top;
    winY = t.top;
    layout();                         /* tinh winW theo cac muc dang bat */
    if (dragging) {
        /* giu nguyen winX */
    } else if (offsetLeft >= 0) {
        winX = t.left + offsetLeft;
    } else {
        winX = t.left + SC(8); /* mac dinh: goc trai taskbar */
    }
    if (winX < t.left) winX = t.left;
    if (winX + winW > t.right) winX = t.right - winW;

    render();
    dst.x = winX; dst.y = winY;
    sz.cx = winW; sz.cy = winH;
    UpdateLayeredWindow(hwnd, NULL, &dst, &sz, memdc, &src, 0, &bf, ULW_ALPHA);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

/* Khi user bam vao taskbar / doi cua so, day app len lai ngay (khong doi 1 giay) */
static void CALLBACK on_foreground(HWINEVENTHOOK h, DWORD ev, HWND w, LONG o, LONG c, DWORD th, DWORD tm)
{
    (void)h; (void)ev; (void)w; (void)o; (void)c; (void)th; (void)tm;
    if (!dragging) update_window();
}

/* ------------------------------------------------------------------ */
/* Cua so                                                              */
/* ------------------------------------------------------------------ */
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_TIMER:
        sample();
        update_window();
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_LBUTTONDOWN: {
        POINT p;
        GetCursorPos(&p);
        dragging = TRUE;
        dragStartCursor = p.x;
        dragStartWin = winX;
        SetCapture(h);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (dragging) {
            POINT p;
            GetCursorPos(&p);
            winX = dragStartWin + (p.x - dragStartCursor);
            update_window();
        }
        return 0;
    case WM_LBUTTONUP:
        if (dragging) {
            RECT t;
            HWND tb = FindWindowW(L"Shell_TrayWnd", NULL);
            dragging = FALSE;
            ReleaseCapture();
            if (tb && GetWindowRect(tb, &t)) {
                offsetLeft = winX - t.left;
                if (offsetLeft < 0) offsetLeft = 0;
                save_offset();
            }
        }
        return 0;
    case WM_RBUTTONUP: {
        POINT p;
        HMENU mn = CreatePopupMenu();
        int cmd;
        int it;
        for (it = 0; it < IT_COUNT; it++)
            AppendMenuW(mn, MF_STRING | ((showMask & (1u << it)) ? MF_CHECKED : 0), IDM_ITEM0 + it, itemMenu[it]);
        AppendMenuW(mn, MF_SEPARATOR, 0, NULL);
        AppendMenuW(mn, MF_STRING | (autostart_get() ? MF_CHECKED : 0), IDM_AUTOSTART,
                    L"Kh\u1edfi \u0111\u1ed9ng c\u00f9ng Windows");
        AppendMenuW(mn, MF_STRING, IDM_RESETPOS, L"\u0110\u1eb7t l\u1ea1i v\u1ecb tr\u00ed");
        AppendMenuW(mn, MF_SEPARATOR, 0, NULL);
        AppendMenuW(mn, MF_STRING, IDM_EXIT, L"Tho\u00e1t");
        GetCursorPos(&p);
        SetForegroundWindow(h);
        cmd = TrackPopupMenu(mn, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, h, NULL);
        DestroyMenu(mn);
        if (cmd >= IDM_ITEM0 && cmd < IDM_ITEM0 + IT_COUNT) {
            DWORD m = showMask ^ (1u << (cmd - IDM_ITEM0));
            if (m) { showMask = m; save_show(); gpu_query_update(); update_window(); } /* khong cho tat het */
        }
        else if (cmd == IDM_AUTOSTART) autostart_set(!autostart_get());
        else if (cmd == IDM_RESETPOS) { offsetLeft = -1; save_offset(); update_window(); }
        else if (cmd == IDM_EXIT) DestroyWindow(h);
        return 0;
    }
    case WM_SETTINGCHANGE:
        load_theme();
        update_window();
        return 0;
    case WM_DISPLAYCHANGE:
        init_layout();
        update_window();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    WNDCLASSW wc;
    MSG msg;
    HANDLE mtx;
    (void)prev; (void)cmd; (void)show;

    mtx = CreateMutexW(NULL, TRUE, L"Local\\TaskbarStats_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    SetProcessDPIAware();

    memdc = CreateCompatibleDC(NULL);
    load_theme();
    load_offset();
    load_show();
    init_layout();
    if (autostart_get()) autostart_set(TRUE); /* cap nhat duong dan neu exe da bi chuyen cho khac */

    pick_gpu();
    init_gpu_temp();
    init_disk();
    init_pdh();
    gpu_query_update();
    sample();

    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = APPNAME;
    RegisterClassW(&wc);

    hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                           APPNAME, APPNAME, WS_POPUP, 0, 0, winW, 40, NULL, NULL, inst, NULL);
    if (!hwnd) return 1;

    SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, on_foreground, 0, 0,
                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    SetTimer(hwnd, 1, 1000, NULL);
    update_window();

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hq) PdhCloseQuery(hq);
    if (hqGpu) PdhCloseQuery(hqGpu);
    if (hDisk != INVALID_HANDLE_VALUE) CloseHandle(hDisk);
    if (mtx) CloseHandle(mtx);
    return 0;
}
