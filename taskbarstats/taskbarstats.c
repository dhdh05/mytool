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
 * Nhieu man hinh: hien tren taskbar cua MOI man (Shell_TrayWnd + Shell_SecondaryTrayWnd).
 * Doc cam bien o luong rieng (sensor_thread): luong giao dien gan voi taskbar khong bao gio bi chan.
 *
 * Thiet bi dang ket noi (menu chuot phai): man hinh + cong (HDMI/DisplayPort/USB-C) qua
 * QueryDisplayConfig; thiet bi USB qua SetupAPI (cay hub), dau Type-C/USB-A qua
 * IOCTL_USB_GET_PORT_CONNECTOR_PROPERTIES. Chi quet khi mo menu, luc chay khong ton gi.
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
#define IDM_EXTDISK   6               /* hien ca o gan ngoai (USB, o cung roi) */
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
    IT_DISK,       /* v1.2: dung luong SSD (gop vao o SSD). De cuoi enum cho khoi lech bit da luu trong registry */
    IT_COUNT
};
/* Thu tu hien tren taskbar va trong menu */
static const int itemOrder[IT_COUNT] = { IT_CPU, IT_RAM, IT_GPU, IT_VRAM, IT_SSD, IT_BAT, IT_DISK };
static const WCHAR *itemMenu[IT_COUNT] = {
    // L"CPU (% + nhi\u1ec7t \u0111\u1ed9)",   /* may khong doc duoc nhiet do CPU */
    L"CPU",
    L"RAM",
    L"GPU (% + nhi\u1ec7t \u0111\u1ed9)",
    L"VRAM",
    L"\u1ed4 SSD",                   /* submenu: Nhiet do / Dung luong / O gan ngoai */
    L"Pin Bluetooth",
    L"Dung l\u01b0\u1ee3ng (\u0111\u00e3 d\u00f9ng / t\u1ed5ng)",
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

/* ---- Luong doc cam bien ----
 * S / batName: chi luong cam bien (sensor_thread) ghi.
 * D / batNameD: ban sao da chot, luong giao dien doc qua V / batNameV.
 * Khoa chi giu trong luc copy vai chuc byte -> luong giao dien khong bao gio phai cho thiet bi. */
typedef struct {
    int cpu, ram, gpu, vram;
    ULONGLONG ramUsed, vramUsed;
    int cpuT, gpuT, ssdT;
    int bat;
} Stats;
static Stats D = { 0, 0, 0, 0, 0, 0, NA, NA, NA, NA }, V = { 0, 0, 0, 0, 0, 0, NA, NA, NA, NA };
static WCHAR batNameD[64] = L"Pin", batNameV[64] = L"Pin";

/* Dung luong o dia
 * sys*  : tong cac phan vung nam tren o vat ly chua Windows (cung o voi nhiet do SSD), vd C: + D: + G:
 * ext   : moi o vat ly khac 1 muc (USB, o cung roi, o thu 2) - chi doc khi bat "Hien ca o gan ngoai" */
#define MAXDRV 8
typedef struct {
    ULONGLONG sysUsed, sysTotal;
    int n;
    WCHAR label[MAXDRV][8];
    ULONGLONG used[MAXDRV], total[MAXDRV];
    BOOL ready;
} Drives;
static Drives drvS, drvD, drvV;       /* S: luong cam bien ghi, D: ban chot, V: luong giao dien doc */
static BOOL   extDisks;               /* hien ca o gan ngoai */
static DWORD  sysDiskNo = 0xFFFFFFFF; /* so o vat ly chua Windows (PhysicalDriveN) */
static CRITICAL_SECTION csStats;
static HANDLE hStop, hSensor;
#define WM_APP_DATA (WM_APP + 1)       /* luong cam bien bao co so lieu moi */

static ULONGLONG pIdle, pKern, pUser;
static PDH_HQUERY   hq;
static PDH_HQUERY   hqGpu;               /* query rieng cho GPU, chi mo khi can */
static PDH_HCOUNTER cGpu, cVram, cTz;
static BOOL tzHighPrec;

static BOOL      gpuFound;
static LUID      gpuLuid;
static UINT      gpuVendor;
static ULONGLONG vramTotal;
static ULONGLONG sharedTotal;         /* bo nho he thong GPU duoc dung chung (DXGI SharedSystemMemory) */
static BOOL      sharedMode;          /* GPU tich hop (khong co VRAM rieng) -> o VRAM do bo nho dung chung */
static WCHAR     luidStr[48];

static PFN_QueryAdapterInfo pQueryAdapterInfo;
static KMT_HANDLE hKmtAdapter;
static PFN_nvmlGetTemp nvmlGetTemp;
static void *nvmlDev;

static HANDLE hDisk = INVALID_HANDLE_VALUE;
static int    tickCount;

/* Giao dien
 * Moi taskbar (Shell_TrayWnd o man chinh + Shell_SecondaryTrayWnd o moi man phu)
 * co 1 cua so overlay rieng, gan lam CUA SO CON cua taskbar do. Nho vay khi mo
 * Start / Search (Windows day taskbar len tang cao hon moi cua so TOPMOST) overlay
 * van nam tren taskbar, khong bi che. Cua so con co alpha (layered child) can
 * manifest khai bao Windows 8+  ->  xem taskbarstats.rc / taskbarstats.manifest.
 * Neu khong tao duoc cua so con thi lui ve cach cu (cua so popup TOPMOST).        */
#define MAXBARS 8
typedef struct {
    HWND hwnd;      /* cua so overlay cua minh */
    HWND tb;        /* taskbar ma no nam tren */
    int  x;         /* vi tri hien tai, toa do man hinh (dung khi keo) */
    BOOL child;     /* TRUE = cua so con cua taskbar, FALSE = popup TOPMOST (du phong) */
    DWORD lastHash; /* noi dung + vi tri lan ve truoc: giong het thi khong ve lai */
    BOOL  drawn;
} Bar;
static Bar     bars[MAXBARS];
static int     nBars;                 /* so slot da tao cua so */
static HWND    hwnd;                  /* cua so an cua app: timer, menu, nhan WM_SETTINGCHANGE... */
static HINSTANCE hInst;
static HFONT   fLabel, fValue;
static HDC     memdc;
static HBITMAP dib;
static DWORD  *bits;
static int     dibW, dibH;
static int     dpi = 96;
static int     winY, winW, winH;
static int     lineH, v1W, v2W, gapV, gapL, colGap, pad;
static int     v2WideW;               /* do rong gia tri 2 kieu "999/999G" (o dia) */
static BOOL    light;
static int     offsetLeft = -1;       /* khoang cach tu mep trai taskbar, -1 = mac dinh (sat goc trai) */
static BOOL    dragging;
static int     dragBar = -1;          /* bar dang bi keo */
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
                sharedTotal = d.SharedSystemMemory;
            }
        }
        IDXGIAdapter1_Release(a);
    }
    IDXGIFactory1_Release(f);
    if (gpuFound)
        wsprintfW(luidStr, L"luid_0x%08X_0x%08X", (UINT)gpuLuid.HighPart, (UINT)gpuLuid.LowPart);
    /* GPU tich hop (Intel Iris Xe, AMD Radeon Graphics...): VRAM rieng chi la vai tram MB firmware giu san,
       GPU thuc te dung RAM may -> do "Shared Usage" giong dong Shared GPU memory trong Task Manager */
    if (gpuFound && vramTotal < (512ULL << 20) && sharedTotal) {
        sharedMode = TRUE;
        vramTotal  = sharedTotal;
        itemMenu[IT_VRAM] = L"B\u1ed9 nh\u1edb GPU (d\u00f9ng chung)";
    }
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
        sysDiskNo = sdn.DeviceNumber;
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
        if (PdhAddEnglishCounterW(hqGpu, sharedMode ? L"\\GPU Adapter Memory(*)\\Shared Usage"
                                                    : L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &cVram) != ERROR_SUCCESS)
            cVram = NULL;
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

/* O vat ly chua phan vung `root` (vd "C:\"). Phan vung trai nhieu o -> lay o dau. 0xFFFFFFFF = khong ro */
static DWORD volume_disk(WCHAR letter)
{
    WCHAR path[8];
    HANDLE v;
    union { VOLUME_DISK_EXTENTS e; BYTE raw[256]; } x;
    DWORD br = 0, r = 0xFFFFFFFF;
    wsprintfW(path, L"\\\\.\\%c:", letter);
    v = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (v == INVALID_HANDLE_VALUE) return r;
    if (DeviceIoControl(v, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, &x, sizeof x, &br, NULL) &&
        x.e.NumberOfDiskExtents > 0)
        r = x.e.Extents[0].DiskNumber;
    CloseHandle(v);
    return r;
}

/* O vat ly co cam qua USB khong (STORAGE_DEVICE_DESCRIPTOR.BusType) */
static BOOL disk_is_usb(DWORD disk)
{
    WCHAR path[32];
    HANDLE h;
    STORAGE_PROPERTY_QUERY q;
    union { STORAGE_DEVICE_DESCRIPTOR d; BYTE raw[512]; } o;
    DWORD br = 0;
    BOOL usb = FALSE;
    wsprintfW(path, L"\\\\.\\PhysicalDrive%u", (UINT)disk);
    h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    ZeroMemory(&q, sizeof q);
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, &o, sizeof o, &br, NULL))
        usb = o.d.BusType == BusTypeUsb;
    CloseHandle(h);
    return usb;
}

static void read_drives(Drives *o, BOOL wantExt)
{
    WCHAR root[4] = L"A:\\";
    DWORD mask = GetLogicalDrives(), extDisk[MAXDRV];
    int i, nUsb = 0;
    o->sysUsed = o->sysTotal = 0;
    o->n = 0;
    for (i = 0; i < 26; i++) {
        ULARGE_INTEGER tot, fre;
        UINT type;
        DWORD disk;
        int k;
        if (!(mask & (1u << i))) continue;
        root[0] = (WCHAR)(L'A' + i);
        type = GetDriveTypeW(root);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;    /* bo o mang, CD, RAM disk */
        disk = volume_disk(root[0]);
        if (disk == 0xFFFFFFFF || disk != sysDiskNo) {
            if (!wantExt) continue;                                      /* o khac: chi doc khi bat */
        }
        if (!GetDiskFreeSpaceExW(root, NULL, &tot, &fre) || !tot.QuadPart) continue;
        if (disk == sysDiskNo) {                                         /* phan vung cua SSD chinh -> cong don */
            o->sysTotal += tot.QuadPart;
            o->sysUsed  += tot.QuadPart - fre.QuadPart;
            continue;
        }
        for (k = 0; k < o->n; k++) if (disk != 0xFFFFFFFF && extDisk[k] == disk) break;   /* gop phan vung cung o */
        if (k == o->n) {
            if (o->n >= MAXDRV) continue;
            extDisk[k] = disk;
            o->used[k] = o->total[k] = 0;
            if (disk != 0xFFFFFFFF && disk_is_usb(disk)) {
                nUsb++;
                if (nUsb == 1) lstrcpyW(o->label[k], L"USB"); else wsprintfW(o->label[k], L"USB %d", nUsb);
            } else {
                wsprintfW(o->label[k], L"%c:", root[0]);                 /* o trong thu 2 / khong ro: ten theo ky tu */
            }
            o->n++;
        }
        o->total[k] += tot.QuadPart;
        o->used[k]  += tot.QuadPart - fre.QuadPart;
    }
    o->ready = TRUE;
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
    {   /* dung luong: 10 giay 1 lan, chi khi dang bat; vua bat / doi tuy chon thi doc ngay */
        static BOOL lastExt = -1;
        BOOL wantExt = extDisks;
        if ((showMask & (1u << IT_DISK)) && (tickCount % 10 == 0 || !drvS.ready || wantExt != lastExt)) {
            read_drives(&drvS, wantExt);
            lastExt = wantExt;
        }
    }
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

static void load_diskmode(void)
{
    DWORD v, sz = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, CFGKEY, L"ExtDisks", RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS)
        extDisks = v ? TRUE : FALSE;
}

static void save_diskmode(void)
{
    HKEY k;
    DWORD v = (DWORD)extDisks;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, CFGKEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return;
    RegSetValueExW(k, L"ExtDisks", 0, REG_DWORD, (BYTE *)&v, sizeof v);
    RegCloseKey(k);
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
    v2WideW = maxi(v2W, maxi(text_w(fValue, L"999/999G"), text_w(fValue, L"9.9/9.9T")));
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
    BOOL wide2;                      /* gia tri 2 dai (vd "178/967G") */
    BOOL wide1;                      /* gia tri 1 dai (o chi co dung luong) */
} Cell;

#define MAXCELLS (IT_COUNT + MAXDRV)
static Cell cells[MAXCELLS];
static int  nCells;
static int  colX[MAXCELLS], colLW[MAXCELLS], colHas2[MAXCELLS], colV1W[MAXCELLS], colV2W[MAXCELLS]; /* theo cot */

/* "178/967G", tren 1000G thi "1.2/1.8T" */
static void fmt_usage(WCHAR *b, ULONGLONG used, ULONGLONG total)
{
    ULONGLONG gu = (used + (1ULL << 29)) >> 30, gt = (total + (1ULL << 29)) >> 30;
    if (gt >= 1000) {
        ULONGLONG tu = (used * 10 + (1ULL << 39)) >> 40, tt = (total * 10 + (1ULL << 39)) >> 40;
        wsprintfW(b, L"%u.%u/%u.%uT", (UINT)(tu / 10), (UINT)(tu % 10), (UINT)(tt / 10), (UINT)(tt % 10));
    } else {
        wsprintfW(b, L"%u/%uG", (UINT)gu, (UINT)gt);
    }
}

static void make_cell(int it, Cell *c)
{
    c->has2 = TRUE;
    c->wide2 = FALSE;
    c->wide1 = FALSE;
    c->s2 = 1;
    switch (it) {
    case IT_CPU:
        c->lab = L"CPU";
        fmt_pct(c->v1, V.cpu);  c->s1 = sev(V.cpu, 70, 90);
        fmt_temp(c->v2, V.cpuT); c->s2 = sev(V.cpuT, 75, 90);
        if (V.cpuT == NA) c->has2 = FALSE;   /* khong doc duoc nhiet do -> chi hien % */
        break;
    case IT_RAM:
        c->lab = L"RAM";
        fmt_pct(c->v1, V.ram);  c->s1 = sev(V.ram, 75, 90);
        fmt_gb(c->v2, V.ramUsed);
        break;
    case IT_GPU:
        c->lab = L"GPU";
        fmt_pct(c->v1, V.gpu);  c->s1 = sev(V.gpu, 70, 90);
        fmt_temp(c->v2, V.gpuT); c->s2 = sev(V.gpuT, 75, 87);
        if (V.gpuT == NA) c->has2 = FALSE;   /* driver khong bao nhiet do -> chi hien % */
        break;
    case IT_VRAM:
        c->lab = sharedMode ? L"GPU RAM" : L"VRAM";
        fmt_pct(c->v1, V.vram); c->s1 = sev(V.vram, 75, 90);
        fmt_gb(c->v2, V.vramUsed);
        break;
    case IT_SSD: {   /* "SSD  30°C  178/967G": nhiet do va/hoac dung luong, tuy menu O SSD */
        BOOL t = (showMask & (1u << IT_SSD)) != 0;
        BOOL u = (showMask & (1u << IT_DISK)) != 0;
        WCHAR use[16];
        int us = 1;
        if (!drvV.ready || !drvV.sysTotal) lstrcpyW(use, L"--");
        else { fmt_usage(use, drvV.sysUsed, drvV.sysTotal); us = sev((int)(drvV.sysUsed * 100 / drvV.sysTotal), 85, 95); }
        c->lab = L"SSD";
        c->has2 = FALSE;
        if (t) {
            fmt_temp(c->v1, V.ssdT); c->s1 = sev(V.ssdT, 60, 70);
            if (u) { lstrcpyW(c->v2, use); c->s2 = us; c->has2 = TRUE; c->wide2 = TRUE; }
        } else {                     /* tat nhiet do: chi con dung luong */
            lstrcpyW(c->v1, use); c->s1 = us; c->wide1 = TRUE;
        }
        break;
    }
    case IT_BAT:
        c->lab = batNameV;        /* ten thiet bi Bluetooth co pin thap nhat */
        if (V.bat == NA) lstrcpyW(c->v1, L"--"); else fmt_pct(c->v1, V.bat);
        c->s1 = sev_bat(V.bat);
        c->has2 = FALSE;
        break;
    }
}

/* Tinh noi dung cac o dang bat va do rong cua so (winW) */
/* O gan ngoai: moi o 1 cell "USB  29/64G" (o cuoi danh sach) */
static void add_ext_cells(void)
{
    int i;
    if (!extDisks || !drvV.ready || !(showMask & (1u << IT_DISK))) return;
    for (i = 0; i < drvV.n && nCells < MAXCELLS; i++) {
        Cell *c = &cells[nCells++];
        c->lab = drvV.label[i];
        fmt_usage(c->v1, drvV.used[i], drvV.total[i]);
        c->s1 = sev(drvV.total[i] ? (int)(drvV.used[i] * 100 / drvV.total[i]) : 0, 85, 95);
        c->v2[0] = 0; c->s2 = 1; c->has2 = FALSE; c->wide2 = FALSE; c->wide1 = TRUE;
    }
}

static void layout(void)
{
    int it, j, x, nCols;
    nCells = 0;
    for (it = 0; it < IT_COUNT; it++) {
        int k = itemOrder[it];
        if (k == IT_DISK) { add_ext_cells(); continue; }          /* dung luong SSD nam trong o SSD */
        if (k == IT_SSD) {
            if (showMask & ((1u << IT_SSD) | (1u << IT_DISK))) make_cell(k, &cells[nCells++]);
            continue;
        }
        if (!(showMask & (1u << k))) continue;
        make_cell(k, &cells[nCells++]);
    }

    nCols = (nCells + 1) / 2;
    x = pad;
    for (j = 0; j < nCols; j++) {
        int r, lw = 0, w;
        colHas2[j] = FALSE;
        colV2W[j] = v2W;
        colV1W[j] = v1W;
        for (r = 0; r < 2 && 2 * j + r < nCells; r++) {
            Cell *c = &cells[2 * j + r];
            lw = maxi(lw, text_w(fLabel, c->lab));
            if (c->has2) colHas2[j] = TRUE;
            if (c->wide2) colV2W[j] = v2WideW;
            if (c->wide1) colV1W[j] = v2WideW;
        }
        if (lw > SC(110)) lw = SC(110);           /* ten dai qua thi cat "..." */
        colLW[j] = lw;
        colX[j] = x;
        w = lw + gapL + colV1W[j] + (colHas2[j] ? gapV + colV2W[j] : 0);
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
            put(x, y, colV1W[j], fValue, c->v1, col[c->s1], DT_RIGHT);
            if (c->has2) put(x + colV1W[j] + gapV, y, colV2W[j], fValue, c->v2, col[c->s2], DT_RIGHT);
        }
    }

    /* alpha = 1 cho nen: gan nhu trong suot nhung van bat duoc chuot */
    for (i = 0; i < dibW * dibH; i++)
        if (!(bits[i] >> 24)) bits[i] = 0x01000000;
}

static int find_bar(HWND w)
{
    int i;
    if (!w) return -1;
    for (i = 0; i < nBars; i++) if (bars[i].hwnd == w) return i;
    return -1;
}

static BOOL is_taskbar_class(const WCHAR *cls)
{
    return !lstrcmpW(cls, L"Shell_TrayWnd") || !lstrcmpW(cls, L"Shell_SecondaryTrayWnd");
}

/* Co app full screen tren dung man hinh 'mon' khong (man khac van hien binh thuong) */
static BOOL fullscreen_app(HMONITOR mon)
{
    HWND fg = GetForegroundWindow();
    WCHAR cls[64];
    RECT r;
    MONITORINFO mi;
    if (!fg || fg == GetDesktopWindow() || fg == GetShellWindow() || fg == hwnd || find_bar(fg) >= 0) return FALSE;
    GetClassNameW(fg, cls, 64);
    if (!lstrcmpW(cls, L"WorkerW") || !lstrcmpW(cls, L"Progman") || is_taskbar_class(cls)) return FALSE;
    /* Start, Search (Win+Q), Notification Center, Widgets... la cua so trong suot phu ca man hinh
       -> khong phai app full screen that */
    if (!lstrcmpW(cls, L"Windows.UI.Core.CoreWindow") ||
        !lstrcmpW(cls, L"XamlExplorerHostIslandWindow") ||
        !lstrcmpW(cls, L"TopLevelWindowForOverflowXamlIsland")) return FALSE;
    if (MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST) != mon) return FALSE;
    GetWindowRect(fg, &r);
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(mon, &mi);
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
           r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
}

/* Gom danh sach taskbar: [0] = man chinh, sau do la cac man phu */
static HWND foundTb[MAXBARS];
static int  nFound;

static BOOL CALLBACK enum_tb(HWND w, LPARAM lp)
{
    WCHAR cls[64];
    (void)lp;
    if (nFound < MAXBARS && GetClassNameW(w, cls, 64) && !lstrcmpW(cls, L"Shell_SecondaryTrayWnd"))
        foundTb[nFound++] = w;
    return TRUE;
}

static void find_taskbars(void)
{
    nFound = 0;
    foundTb[nFound++] = FindWindowW(L"Shell_TrayWnd", NULL);   /* co the NULL khi Explorer dang khoi dong lai */
    EnumWindows(enum_tb, 0);
}

/* Tao overlay cho taskbar 'tb': uu tien cua so con cua taskbar, that bai thi dung popup TOPMOST */
static void create_overlay(Bar *b, HWND tb)
{
    b->tb = tb;
    b->x = 0;
    b->drawn = FALSE;
    b->child = TRUE;
    b->hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                              APPNAME, NULL, WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 10, 10, tb, NULL, hInst, NULL);
    if (!b->hwnd) {                   /* vd. exe khong co manifest -> layered child bi tu choi */
        b->child = FALSE;
        b->hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                                  APPNAME, APPNAME, WS_POPUP, 0, 0, 10, 10, NULL, NULL, hInst, NULL);
    }
}

static void destroy_overlay(Bar *b)
{
    if (b->hwnd && IsWindow(b->hwnd)) DestroyWindow(b->hwnd);
    b->hwnd = NULL;
    b->tb = NULL;
}

/* Bam FNV-1a: dau van tay noi dung + vi tri, de bo qua lan ve khong can thiet */
static DWORD fnv(DWORD h, const void *p, size_t n)
{
    const BYTE *q = (const BYTE *)p;
    while (n--) { h ^= *q++; h *= 16777619u; }
    return h;
}

static DWORD frame_hash(int x, int y)
{
    DWORD h = 2166136261u;
    int i;
    for (i = 0; i < nCells; i++) {
        h = fnv(h, cells[i].lab, (size_t)lstrlenW(cells[i].lab) * sizeof(WCHAR));
        h = fnv(h, cells[i].v1, sizeof cells[i].v1);
        h = fnv(h, cells[i].v2, sizeof cells[i].v2);
        h = fnv(h, &cells[i].s1, sizeof(int) * 2);
        h = fnv(h, &cells[i].has2, sizeof(BOOL) * 3);
    }
    h = fnv(h, &x, sizeof x);   h = fnv(h, &y, sizeof y);
    h = fnv(h, &winW, sizeof winW); h = fnv(h, &winH, sizeof winH);
    h = fnv(h, col, sizeof col); h = fnv(h, &lineH, sizeof lineH);
    return h;
}

/* Ve va dat 1 overlay len taskbar cua no */
static void update_bar(int i)
{
    Bar *b = &bars[i];
    RECT t;
    MONITORINFO mi;
    HMONITOR mon;
    POINT dst, src = { 0, 0 };
    SIZE sz;
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };

    if (!b->hwnd) return;
    if (!b->tb || !IsWindow(b->tb) || !GetWindowRect(b->tb, &t)) { ShowWindow(b->hwnd, SW_HIDE); b->drawn = FALSE; return; }
    mon = MonitorFromWindow(b->tb, MONITOR_DEFAULTTONEAREST);
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(mon, &mi);

    /* An khi: taskbar doc, taskbar dang tu an, hoac co app full screen tren man nay */
    if ((t.bottom - t.top) > (t.right - t.left) ||
        t.top >= mi.rcMonitor.bottom - SC(4) || t.bottom <= mi.rcMonitor.top + SC(4) ||
        fullscreen_app(mon)) {
        ShowWindow(b->hwnd, SW_HIDE);
        b->drawn = FALSE;
        return;
    }

    winH = t.bottom - t.top;
    winY = t.top;
    layout();                         /* tinh winW theo cac muc dang bat */
    if (dragging && dragBar == i) {
        /* giu nguyen b->x */
    } else if (offsetLeft >= 0) {
        b->x = t.left + offsetLeft;   /* cung khoang cach tu mep trai tren moi taskbar */
    } else {
        b->x = t.left + SC(8);        /* mac dinh: goc trai taskbar */
    }
    if (b->x + winW > t.right) b->x = t.right - winW;
    if (b->x < t.left) b->x = t.left;

    sz.cx = winW; sz.cy = winH;
    {
        DWORD h = frame_hash(b->x, winY);
        if (b->drawn && h == b->lastHash && IsWindowVisible(b->hwnd)) {
            /* khong co gi thay doi: khoi ve lai, chi giu overlay nam tren cung */
            if (b->child) {
                if (GetTopWindow(b->tb) != b->hwnd)
                    SetWindowPos(b->hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            } else {
                SetWindowPos(b->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
            return;
        }
        b->lastHash = h;
        b->drawn = TRUE;
    }
    render();
    if (b->child) {
        /* cua so con: vi tri tinh theo toa do client cua taskbar, dat len tren cac cua so con khac (XAML) */
        dst.x = b->x; dst.y = winY;
        ScreenToClient(b->tb, &dst);
        UpdateLayeredWindow(b->hwnd, NULL, NULL, &sz, memdc, &src, 0, &bf, ULW_ALPHA);
        SetWindowPos(b->hwnd, HWND_TOP, dst.x, dst.y, winW, winH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
        dst.x = b->x; dst.y = winY;
        UpdateLayeredWindow(b->hwnd, NULL, &dst, &sz, memdc, &src, 0, &bf, ULW_ALPHA);
        SetWindowPos(b->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

/* Tim lai cac taskbar (cam/rut man hinh), tao them overlay neu can, cap nhat tat ca */
static ULONGLONG nextScan;            /* thoi diem quet lai taskbar dinh ky (ms) */
static ULONGLONG burstUntil;          /* sau khi cam/rut man: quet moi lan cap nhat toi thoi diem nay */
static UINT msgTaskbarCreated;        /* Explorer khoi dong lai */

static BOOL bars_valid(void)
{
    int i;
    for (i = 0; i < nBars; i++)
        if (bars[i].tb && (!IsWindow(bars[i].tb) || !bars[i].hwnd || !IsWindow(bars[i].hwnd))) return FALSE;
    return nBars > 0 && bars[0].tb;
}

static void update_window(void)
{
    int i;
    /* lay ban sao so lieu moi nhat (giu khoa vai micro giay) */
    EnterCriticalSection(&csStats);
    V = D;
    lstrcpyW(batNameV, batNameD);
    drvV = drvD;
    LeaveCriticalSection(&csStats);

    /* Chi quet lai danh sach taskbar khi can: cam/rut man, Explorer khoi dong lai,
       handle chet, hoac moi 5 giay mot lan de du phong. */
    if (!dragging) {
        ULONGLONG now = GetTickCount64();
        if (now >= nextScan || now < burstUntil || !bars_valid()) {
            nextScan = now + 5000;
            find_taskbars();
            for (i = 0; i < nFound; i++) {
                /* taskbar moi, hoac overlay bi huy theo taskbar (Explorer khoi dong lai) -> tao lai */
                if (bars[i].tb != foundTb[i] || !bars[i].hwnd || !IsWindow(bars[i].hwnd)) {
                    destroy_overlay(&bars[i]);
                    if (foundTb[i]) create_overlay(&bars[i], foundTb[i]);
                }
            }
            for (; i < nBars; i++) destroy_overlay(&bars[i]);   /* man phu da rut ra */
            nBars = nFound;
        }
    }
    for (i = 0; i < nBars; i++) update_bar(i);
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
/* Luong cam bien: khoi tao + doc moi giay, khong bao gio dung toi cua so.
   Neu 1 lan doc bi treo thi chi so lieu dung cap nhat, taskbar va giao dien van muot. */
static DWORD WINAPI sensor_thread(LPVOID arg)
{
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    pick_gpu();
    init_gpu_temp();
    init_disk();
    init_pdh();
    do {
        gpu_query_update();           /* mo/dong query GPU theo menu - chi luong nay dung toi PDH */
        sample();
        EnterCriticalSection(&csStats);
        D.cpu = S.cpu; D.ram = S.ram; D.gpu = S.gpu; D.vram = S.vram;
        D.ramUsed = S.ramUsed; D.vramUsed = S.vramUsed;
        D.cpuT = S.cpuT; D.gpuT = S.gpuT; D.ssdT = S.ssdT; D.bat = S.bat;
        lstrcpyW(batNameD, batName);
        drvD = drvS;
        LeaveCriticalSection(&csStats);
        PostMessageW(hwnd, WM_APP_DATA, 0, 0);    /* khong cho, giao dien tu ve khi ranh */
    } while (WaitForSingleObject(hStop, 1000) == WAIT_TIMEOUT);
    if (hq) PdhCloseQuery(hq);
    if (hqGpu) PdhCloseQuery(hqGpu);
    if (hDisk != INVALID_HANDLE_VALUE) CloseHandle(hDisk);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Thiet bi dang ket noi (chi quet khi mo menu -> khong ton RAM/CPU luc chay) */
/* ------------------------------------------------------------------ */
/* DEVPKEY tu khai bao (khoi can initguid / cfgmgr32) */
static const DEVPROPKEY PKEY_BusDesc =      /* DEVPKEY_Device_BusReportedDeviceDesc: ten san pham do thiet bi bao */
    { { 0x540B947E, 0x8B40, 0x45BC, { 0xA8, 0xA2, 0x6A, 0x0B, 0x89, 0x4C, 0xBD, 0xA2 } }, 4 };
static const DEVPROPKEY PKEY_Parent =       /* DEVPKEY_Device_Parent: instance ID cua node cha */
    { { 0x4340A6C5, 0x93FA, 0x4706, { 0x97, 0x2C, 0x7B, 0x64, 0x80, 0x08, 0xA5, 0xA7 } }, 8 };
static const DEVPROPKEY PKEY_Address =      /* DEVPKEY_Device_Address: voi thiet bi USB = so cong tren hub */
    { { 0xA45C254E, 0xDF1C, 0x4EFD, { 0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0 } }, 30 };
static const GUID GUID_IF_USB_HUB =         /* GUID_DEVINTERFACE_USB_HUB */
    { 0xF18A0E88, 0xC30C, 0x11D0, { 0x88, 0x15, 0x00, 0xA0, 0xC9, 0x06, 0xBE, 0xD8 } };

/* IOCTL_USB_GET_PORT_CONNECTOR_PROPERTIES (usbioctl.h, Win8+) - tu khai bao */
#define IOCTL_USB_PORT_CONNECTOR_PROPS CTL_CODE(0x00000022 /* FILE_DEVICE_USB */, 281, METHOD_BUFFERED, FILE_ANY_ACCESS)
typedef struct {
    ULONG  ConnectionIndex;
    ULONG  ActualLength;
    ULONG  PortProps;          /* bit0 UserConnectable, bit3 ConnectorIsTypeC */
    USHORT CompanionIndex;
    USHORT CompanionPortNumber;
    WCHAR  CompanionHubSymbolicLinkName[1];
} USB_PORT_CONN_PROPS;

#ifndef CM_DEVCAP_REMOVABLE
#define CM_DEVCAP_REMOVABLE 0x00000004  /* cfgmgr32.h */
#endif
#define MAXDEV 64
typedef struct {
    WCHAR id[200], parent[200], name[80];
    BOOL  hub, removable;
    ULONG port;
} UsbDev;
static UsbDev *devs;
static int     nDevs;

static BOOL get_str(HDEVINFO h, SP_DEVINFO_DATA *d, const DEVPROPKEY *k, WCHAR *out, DWORD cch)
{
    DEVPROPTYPE t = 0;
    DWORD sz = 0;
    out[0] = 0;
    if (!SetupDiGetDevicePropertyW(h, d, k, &t, (BYTE *)out, cch * sizeof(WCHAR), &sz, 0) || t != DEVPROP_TYPE_STRING) {
        out[0] = 0;
        return FALSE;
    }
    out[cch - 1] = 0;
    return out[0] != 0;
}

static WCHAR up(WCHAR c) { return (c >= L'a' && c <= L'z') ? (WCHAR)(c - 32) : c; }
static BOOL starts_with(const WCHAR *s, const WCHAR *p)   /* so sanh dau chuoi, khong phan biet hoa thuong */
{
    while (*p) if (up(*s++) != up(*p++)) return FALSE;
    return TRUE;
}

/* Lay danh sach node USB\VID_... (bo interface con &MI_) */
static void scan_usb(void)
{
    HDEVINFO h;
    SP_DEVINFO_DATA d;
    DWORD i;
    nDevs = 0;
    if (!devs) devs = (UsbDev *)HeapAlloc(GetProcessHeap(), 0, sizeof(UsbDev) * MAXDEV);
    if (!devs) return;
    h = SetupDiGetClassDevsW(NULL, L"USB", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (h == INVALID_HANDLE_VALUE) return;
    d.cbSize = sizeof d;
    for (i = 0; SetupDiEnumDeviceInfo(h, i, &d) && nDevs < MAXDEV; i++) {
        UsbDev *u = &devs[nDevs];
        WCHAR svc[32];
        DWORD caps = 0, t = 0, sz = 0;
        if (!SetupDiGetDeviceInstanceIdW(h, &d, u->id, 200, NULL)) continue;
        if (!starts_with(u->id, L"USB\\VID_") || wcsstr(u->id, L"&MI_")) continue;
        get_str(h, &d, &PKEY_Parent, u->parent, 200);
        /* ten: chuoi san pham thiet bi tu bao (vd "USB Receiver"), roi toi FriendlyName, DeviceDesc */
        if (!get_str(h, &d, &PKEY_BusDesc, u->name, 80) &&
            !get_str(h, &d, &PKEY_FriendlyName, u->name, 80) &&
            !get_str(h, &d, &PKEY_DeviceDesc, u->name, 80))
            lstrcpyW(u->name, L"Thiết bị USB");
        svc[0] = 0;
        SetupDiGetDeviceRegistryPropertyW(h, &d, SPDRP_SERVICE, NULL, (BYTE *)svc, sizeof svc, NULL);
        u->hub = !lstrcmpiW(svc, L"USBHUB3") || !lstrcmpiW(svc, L"usbhub") || !lstrcmpiW(svc, L"USBHUB");
        SetupDiGetDeviceRegistryPropertyW(h, &d, SPDRP_CAPABILITIES, NULL, (BYTE *)&caps, sizeof caps, NULL);
        u->removable = (caps & CM_DEVCAP_REMOVABLE) != 0;  /* thiet bi gan trong may (webcam, BT...) thuong = 0 */
        u->port = 0;
        SetupDiGetDevicePropertyW(h, &d, &PKEY_Address, &t, (BYTE *)&u->port, sizeof u->port, &sz, 0);
        nDevs++;
    }
    SetupDiDestroyDeviceInfoList(h);
}

/* Cong `port` tren hub `hubId` co phai dau Type-C khong (do firmware/ACPI khai bao). -1 = khong ro */
static int port_is_typec(const WCHAR *hubId, ULONG port)
{
    HDEVINFO h;
    SP_DEVICE_INTERFACE_DATA ifd;
    union { SP_DEVICE_INTERFACE_DETAIL_DATA_W d; BYTE raw[512]; } det;
    int r = -1;
    if (!port) return -1;
    h = SetupDiGetClassDevsW(&GUID_IF_USB_HUB, hubId, NULL, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (h == INVALID_HANDLE_VALUE) return -1;
    ifd.cbSize = sizeof ifd;
    det.d.cbSize = sizeof det.d;
    if (SetupDiEnumDeviceInterfaces(h, NULL, &GUID_IF_USB_HUB, 0, &ifd) &&
        SetupDiGetDeviceInterfaceDetailW(h, &ifd, &det.d, sizeof det, NULL, NULL)) {
        HANDLE f = CreateFileW(det.d.DevicePath, GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (f == INVALID_HANDLE_VALUE)
            f = CreateFileW(det.d.DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            union { USB_PORT_CONN_PROPS p; BYTE raw[256]; } q;
            DWORD br = 0;
            ZeroMemory(&q, sizeof q);
            q.p.ConnectionIndex = port;
            if (DeviceIoControl(f, IOCTL_USB_PORT_CONNECTOR_PROPS, &q, sizeof q, &q, sizeof q, &br, NULL))
                r = (q.p.PortProps & 8) ? 1 : 0;
            CloseHandle(f);
        }
    }
    SetupDiDestroyDeviceInfoList(h);
    return r;
}

static void add_info(HMENU m, const WCHAR *s) { AppendMenuW(m, MF_STRING, 0, s); }
static void add_head(HMENU m, const WCHAR *s) { AppendMenuW(m, MF_STRING | MF_GRAYED, 0, s); }

/* Them cac thiet bi con cua node `parentId` vao menu. depth = muc thut le (qua bao nhieu hub) */
static int add_usb_children(HMENU m, const WCHAR *parentId, int depth, BOOL onRootHub, int *count)
{
    int i, added = 0;
    for (i = 0; i < nDevs; i++) {
        UsbDev *u = &devs[i];
        WCHAR line[160];
        if (lstrcmpiW(u->parent, parentId)) continue;
        if (!u->removable && !u->hub) continue;        /* bo thiet bi gan trong may */
        if (!u->removable && u->hub) {                   /* hub gan trong may: khong hien, nhung xet con cua no */
            added += add_usb_children(m, u->id, depth, FALSE, count);
            continue;
        }
        {
            const WCHAR *ind = depth == 0 ? L"" : depth == 1 ? L"      ↳ " : L"            ↳ ";
            const WCHAR *tag = L"";
            if (depth == 0 && onRootHub) {
                int tc = port_is_typec(u->parent, u->port);
                if (tc == 1) tag = L"   [Type-C]";
                else if (tc == 0) tag = L"   [USB-A]";
            }
            if (u->hub) wsprintfW(line, L"%s%s%s", ind, L"USB hub", tag);
            else        wsprintfW(line, L"%s%s%s", ind, u->name, tag);
            add_info(m, line);
            added++;
            if (!u->hub) (*count)++;
        }
        if (u->hub && depth < 2) {
            int before = added;
            added += add_usb_children(m, u->id, depth + 1, FALSE, count);
            if (added == before) add_info(m, depth == 0 ? L"      ↳ (trống)" : L"            ↳ (trống)");
        }
    }
    return added;
}

static const WCHAR *output_name(int tech)
{
    switch (tech) {
    case 5:  return L"HDMI";
    case 10: return L"DisplayPort / USB-C";
    case 18: return L"USB-C (USB4/Thunderbolt)";
    case 4:  return L"DVI";
    case 0:  return L"VGA";
    case 15: return L"Miracast (không dây)";
    case 16: return L"USB (DisplayLink)";
    case 6: case 11: case 13: case (int)0x80000000: return NULL;   /* man hinh laptop */
    default: return L"Khác";
    }
}

/* Man hinh dang bat: ten + cong (HDMI / DisplayPort / USB-C...) */
static void add_monitors(HMENU m)
{
    UINT32 np = 0, nm = 0, i;
    DISPLAYCONFIG_PATH_INFO *paths = NULL;
    DISPLAYCONFIG_MODE_INFO *modes = NULL;
    int shown = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) == ERROR_SUCCESS && np) {
        paths = (DISPLAYCONFIG_PATH_INFO *)HeapAlloc(GetProcessHeap(), 0, np * sizeof *paths);
        modes = (DISPLAYCONFIG_MODE_INFO *)HeapAlloc(GetProcessHeap(), 0, (nm ? nm : 1) * sizeof *modes);
        if (paths && modes && QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL) == ERROR_SUCCESS) {
            for (i = 0; i < np; i++) {
                DISPLAYCONFIG_TARGET_DEVICE_NAME tn;
                WCHAR line[160];
                const WCHAR *conn;
                ZeroMemory(&tn, sizeof tn);
                tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
                tn.header.size = sizeof tn;
                tn.header.adapterId = paths[i].targetInfo.adapterId;
                tn.header.id = paths[i].targetInfo.id;
                if (DisplayConfigGetDeviceInfo(&tn.header) != ERROR_SUCCESS) continue;
                conn = output_name((int)tn.outputTechnology);
                if (!conn)
                    wsprintfW(line, L"%s   [màn laptop]", tn.monitorFriendlyDeviceName[0] ? tn.monitorFriendlyDeviceName
                                                                                  : L"Màn hình tích hợp");
                else
                    wsprintfW(line, L"%s   [%s]", tn.monitorFriendlyDeviceName[0] ? tn.monitorFriendlyDeviceName
                                                                               : L"Màn hình", conn);
                add_info(m, line);
                shown++;
            }
        }
    }
    if (paths) HeapFree(GetProcessHeap(), 0, paths);
    if (modes) HeapFree(GetProcessHeap(), 0, modes);
    if (!shown) add_info(m, L"(không đọc được)");
}

/* Tao submenu "Thiet bi dang ket noi" - goi moi lan mo menu chuot phai */
static HMENU build_devices_menu(void)
{
    HMENU m = CreatePopupMenu();
    int i, count = 0, shownRoots = 0;
    add_head(m, L"MÀN HÌNH");
    add_monitors(m);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    add_head(m, L"USB");
    scan_usb();
    /* duyet tung root hub (node cha bat dau bang USB\ROOT_HUB) */
    for (i = 0; i < nDevs; i++) {
        int j, dup = 0;
        if (!starts_with(devs[i].parent, L"USB\\ROOT_HUB")) continue;
        for (j = 0; j < i; j++) if (!lstrcmpiW(devs[j].parent, devs[i].parent)) { dup = 1; break; }
        if (dup) continue;
        shownRoots += add_usb_children(m, devs[i].parent, 0, TRUE, &count);
    }
    if (!shownRoots) add_info(m, L"(không có thiết bị USB cắm ngoài)");
    return m;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_APP_DATA:                 /* so lieu moi tu luong cam bien */
    case WM_TIMER:                    /* du phong: van cap nhat vi tri du luong cam bien bi treo */
        if (!dragging) update_window();
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_LBUTTONDOWN: {
        POINT p;
        int i = find_bar(h);
        if (i < 0) return 0;
        GetCursorPos(&p);
        dragging = TRUE;
        dragBar = i;
        dragStartCursor = p.x;
        dragStartWin = bars[i].x;
        SetCapture(h);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (dragging && dragBar >= 0) {
            POINT p;
            GetCursorPos(&p);
            bars[dragBar].x = dragStartWin + (p.x - dragStartCursor);
            update_bar(dragBar);
        }
        return 0;
    case WM_LBUTTONUP:
        if (dragging) {
            RECT t;
            int i = dragBar;
            dragging = FALSE;
            dragBar = -1;
            ReleaseCapture();
            /* luu khoang cach tu mep trai -> ap dung cho tat ca taskbar */
            if (i >= 0 && bars[i].tb && GetWindowRect(bars[i].tb, &t)) {
                offsetLeft = bars[i].x - t.left;
                if (offsetLeft < 0) offsetLeft = 0;
                save_offset();
            }
            update_window();
        }
        return 0;
    case WM_RBUTTONUP: {
        POINT p;
        HMENU mn = CreatePopupMenu();
        int cmd;
        int it;
        AppendMenuW(mn, MF_POPUP, (UINT_PTR)build_devices_menu(), L"Thi\u1ebft b\u1ecb \u0111ang k\u1ebft n\u1ed1i");
        AppendMenuW(mn, MF_SEPARATOR, 0, NULL);
        for (it = 0; it < IT_COUNT; it++) {
            int k = itemOrder[it];
            UINT on = (showMask & (1u << k)) ? MF_CHECKED : 0;
            if (k == IT_DISK) continue;    /* nam trong submenu O SSD */
            if (k == IT_SSD) {              /* O SSD: Nhiet do / Dung luong / O gan ngoai */
                HMENU dm = CreatePopupMenu();
                UINT onU = (showMask & (1u << IT_DISK)) ? MF_CHECKED : 0;
                AppendMenuW(dm, MF_STRING | on, IDM_ITEM0 + IT_SSD, L"Nhi\u1ec7t \u0111\u1ed9");
                AppendMenuW(dm, MF_STRING | onU, IDM_ITEM0 + IT_DISK, itemMenu[IT_DISK]);
                AppendMenuW(dm, MF_SEPARATOR, 0, NULL);
                AppendMenuW(dm, MF_STRING | (extDisks ? MF_CHECKED : 0), IDM_EXTDISK,
                            L"Hi\u1ec7n c\u1ea3 \u1ed5 g\u1eafn ngo\u00e0i (USB, \u1ed5 c\u1ee9ng r\u1eddi)");
                AppendMenuW(mn, MF_POPUP | ((on || onU) ? MF_CHECKED : 0), (UINT_PTR)dm, itemMenu[IT_SSD]);
            } else {
                AppendMenuW(mn, MF_STRING | on, IDM_ITEM0 + k, itemMenu[k]);
            }
        }
        AppendMenuW(mn, MF_SEPARATOR, 0, NULL);
        AppendMenuW(mn, MF_STRING | (autostart_get() ? MF_CHECKED : 0), IDM_AUTOSTART,
                    L"Kh\u1edfi \u0111\u1ed9ng c\u00f9ng Windows");
        AppendMenuW(mn, MF_STRING, IDM_RESETPOS, L"\u0110\u1eb7t l\u1ea1i v\u1ecb tr\u00ed");
        AppendMenuW(mn, MF_SEPARATOR, 0, NULL);
        AppendMenuW(mn, MF_STRING, IDM_EXIT, L"Tho\u00e1t");
        GetCursorPos(&p);
        SetForegroundWindow(hwnd);
        cmd = TrackPopupMenu(mn, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, hwnd, NULL);
        PostMessageW(hwnd, WM_NULL, 0, 0);
        DestroyMenu(mn);
        if (cmd >= IDM_ITEM0 && cmd < IDM_ITEM0 + IT_COUNT) {
            DWORD m = showMask ^ (1u << (cmd - IDM_ITEM0));
            if (m) { showMask = m; save_show(); update_window(); } /* khong cho tat het */
        }
        else if (cmd == IDM_EXTDISK) {
            extDisks = !extDisks;
            save_diskmode();
            if (extDisks && !(showMask & (1u << IT_DISK))) { showMask |= 1u << IT_DISK; save_show(); }
            update_window();
        }
        else if (cmd == IDM_AUTOSTART) autostart_set(!autostart_get());
        else if (cmd == IDM_RESETPOS) { offsetLeft = -1; save_offset(); update_window(); }
        else if (cmd == IDM_EXIT) DestroyWindow(hwnd);   /* dong cua so chinh -> thoat */
        return 0;
    }
    case WM_SETTINGCHANGE:
        load_theme();
        update_window();
        return 0;
    case WM_DISPLAYCHANGE:
        init_layout();
        burstUntil = GetTickCount64() + 3000;   /* taskbar man phu duoc tao tre 0.5-2s sau khi cam */
        update_window();
        return 0;
    case WM_DESTROY:
        if (h == hwnd) {                     /* chi cua so an cua app moi thoat app */
            int i;
            for (i = 0; i < nBars; i++) destroy_overlay(&bars[i]);
            PostQuitMessage(0);
        }
        return 0;
    }
    if (m == msgTaskbarCreated && msgTaskbarCreated) {   /* Explorer vua khoi dong lai */
        burstUntil = GetTickCount64() + 3000;
        update_window();
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
    load_diskmode();
    init_layout();
    if (autostart_get()) autostart_set(TRUE); /* cap nhat duong dan neu exe da bi chuyen cho khac */

    InitializeCriticalSection(&csStats);
    msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = APPNAME;
    RegisterClassW(&wc);

    hInst = inst;
    /* cua so an: khong hien, chi de giu timer / menu / nhan thong bao he thong */
    hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, APPNAME, APPNAME, WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!hwnd) return 1;

    SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, on_foreground, 0, 0,
                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    hStop = CreateEventW(NULL, TRUE, FALSE, NULL);
    hSensor = CreateThread(NULL, 0, sensor_thread, NULL, 0, NULL);
    SetTimer(hwnd, 1, 1000, NULL);
    update_window();

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    /* bao luong cam bien dung; cho toi da 2 giay, treo thi de Windows don khi thoat process */
    if (hStop) SetEvent(hStop);
    if (hSensor) { WaitForSingleObject(hSensor, 2000); CloseHandle(hSensor); }
    if (mtx) CloseHandle(mtx);
    return 0;
}
