/* windows.h para Android — el subconjunto de Win32 que usa el port de Project Baseball, sobre
 * pthreads y POSIX. Así los fuentes del port (fuente\port, copia del PC) compilan casi sin cambios.
 * Lo que no está aquí no lo usa el port en Android (las ventanas Win32 del launcher se sustituyen
 * por versiones propias de Android).
 *
 * Rutas: el port arma rutas con '\'. Las funciones de archivos de aquí (y fopen, vía macro) las
 * convierten a '/'. */
#ifndef PB_WINDOWS_COMPAT_H
#define PB_WINDOWS_COMPAT_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- tipos --- */
typedef int BOOL;
typedef unsigned char BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef uint32_t ULONG;
typedef int64_t LONGLONG;
typedef uint64_t ULONGLONG;
typedef int64_t LONG64;
typedef unsigned int UINT;
typedef int INT;
typedef char CHAR;
typedef uint16_t WCHAR;
typedef size_t SIZE_T;
typedef uintptr_t ULONG_PTR, DWORD_PTR, UINT_PTR;
typedef intptr_t LONG_PTR, INT_PTR;
typedef void *LPVOID, *PVOID, *HANDLE, *HMODULE, *HINSTANCE, *HWND, *HDC, *HKEY;
typedef const void *LPCVOID;
typedef char *LPSTR;
typedef const char *LPCSTR;
typedef WCHAR *LPWSTR;
typedef const WCHAR *LPCWSTR;
typedef DWORD *LPDWORD;
typedef BOOL *LPBOOL;
typedef void (*FARPROC)(void);
typedef DWORD (*LPTHREAD_START_ROUTINE)(LPVOID);
typedef struct { DWORD nLength; LPVOID lpSecurityDescriptor; BOOL bInheritHandle; } SECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;
typedef union { struct { DWORD LowPart; LONG HighPart; }; struct { DWORD LowPart; LONG HighPart; } u; LONGLONG QuadPart; } LARGE_INTEGER;
typedef union { struct { DWORD LowPart; DWORD HighPart; }; ULONGLONG QuadPart; } ULARGE_INTEGER;
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;
typedef struct { pthread_mutex_t m; int ini; } CRITICAL_SECTION, *LPCRITICAL_SECTION;
typedef struct { pthread_cond_t c; } CONDITION_VARIABLE, *PCONDITION_VARIABLE;
typedef struct { DWORD dwNumberOfProcessors; DWORD dwPageSize; } SYSTEM_INFO;
typedef struct { DWORD dwLength; DWORD dwMemoryLoad; ULONGLONG ullTotalPhys, ullAvailPhys, ullTotalPageFile, ullAvailPageFile, ullTotalVirtual, ullAvailVirtual, ullAvailExtendedVirtual; } MEMORYSTATUSEX;

#define WINAPI
#define CALLBACK
#define APIENTRY
#define TRUE 1
#define FALSE 0
#define MAX_PATH 260
#define INFINITE 0xFFFFFFFFu
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u
#define WAIT_FAILED 0xFFFFFFFFu
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define ERROR_SUCCESS 0
#define ERROR_TIMEOUT 1460
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_ALREADY_EXISTS 183
#define STILL_ACTIVE 259
#define DUPLICATE_SAME_ACCESS 2
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define NORMAL_PRIORITY_CLASS 0x20
#define ABOVE_NORMAL_PRIORITY_CLASS 0x8000
#define HIGH_PRIORITY_CLASS 0x80
#define MB_OK 0
#define MB_ICONERROR 0x10
#define MB_ICONWARNING 0x30
#define MB_ICONINFORMATION 0x40
#define MB_YESNO 4
#define IDOK 1
#define IDYES 6
#define IDNO 7
#define LOWORD(l) ((WORD)((DWORD_PTR)(l) & 0xFFFF))
#define HIWORD(l) ((WORD)(((DWORD_PTR)(l) >> 16) & 0xFFFF))

/* __declspec(...) de MSVC */
#define __declspec(x) __pb_declspec_##x
#define __pb_declspec_thread __thread
#define __pb_declspec_dllexport __attribute__((visibility("default")))
#define __pb_declspec_dllimport
#define __pb_declspec_noinline __attribute__((noinline))
#define __pb_declspec_noreturn __attribute__((noreturn))
#define __forceinline inline __attribute__((always_inline))

/* --- CRT de MSVC --- */
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define stricmp strcasecmp
#define _strdup strdup
#define _snprintf snprintf
#define _vsnprintf vsnprintf
#define _fileno fileno
#define _dup2 dup2
#define _unlink unlink
#define _fseeki64 fseeko
#define _ftelli64 ftello
static inline int _putenv(const char *s) { char *c = strdup(s); return c ? putenv(c) : -1; }
static inline int _putenv_s(const char *n, const char *v) { return setenv(n, v, 1); }

/* --- tiempo --- */
/* QueryPerformanceCounter = contador del procesador (cntvct_el0, 19,2 MHz en Snapdragon): se llama
 * varias veces por cada dibujo (cronómetros de gpu_vk.c) y clock_gettime costaba el 8 % del hilo de
 * dibujo en el perfil del partido. */
#if defined(__aarch64__)
static inline BOOL QueryPerformanceFrequency(LARGE_INTEGER *f) { uint64_t v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); f->QuadPart = (LONGLONG)v; return TRUE; }
static inline BOOL QueryPerformanceCounter(LARGE_INTEGER *c) { uint64_t v; __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v)); c->QuadPart = (LONGLONG)v; return TRUE; }
#else
static inline BOOL QueryPerformanceFrequency(LARGE_INTEGER *f) { f->QuadPart = 1000000000LL; return TRUE; }
static inline BOOL QueryPerformanceCounter(LARGE_INTEGER *c) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); c->QuadPart = (LONGLONG)t.tv_sec * 1000000000LL + t.tv_nsec; return TRUE;
}
#endif
static inline ULONGLONG GetTickCount64(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (ULONGLONG)t.tv_sec * 1000ULL + (ULONGLONG)(t.tv_nsec / 1000000); }
static inline DWORD GetTickCount(void) { return (DWORD)GetTickCount64(); }
static inline void Sleep(DWORD ms) {
    if (!ms) { sched_yield(); return; }
    struct timespec t = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    while (nanosleep(&t, &t) == -1 && errno == EINTR) {}
}
static inline BOOL SwitchToThread(void) { sched_yield(); return TRUE; }
static inline UINT timeBeginPeriod(UINT p) { (void)p; return 0; }
static inline UINT timeEndPeriod(UINT p) { (void)p; return 0; }
void GetLocalTime(SYSTEMTIME *st);
void GetSystemTime(SYSTEMTIME *st);
void GetSystemTimeAsFileTime(FILETIME *ft);

/* --- atómicos --- */
static inline LONG InterlockedIncrement(volatile LONG *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedDecrement(volatile LONG *p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedExchange(volatile LONG *p, LONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedExchangeAdd(volatile LONG *p, LONG v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedCompareExchange(volatile LONG *p, LONG v, LONG cmp) { __atomic_compare_exchange_n(p, &cmp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return cmp; }
static inline LONG64 InterlockedIncrement64(volatile LONG64 *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline LONG64 InterlockedExchangeAdd64(volatile LONG64 *p, LONG64 v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
static inline LONG64 InterlockedCompareExchange64(volatile LONG64 *p, LONG64 v, LONG64 cmp) { __atomic_compare_exchange_n(p, &cmp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return cmp; }
static inline PVOID InterlockedCompareExchangePointer(PVOID volatile *p, PVOID v, PVOID cmp) { __atomic_compare_exchange_n(p, &cmp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return cmp; }
#define MemoryBarrier() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define YieldProcessor() __asm__ __volatile__("yield")

/* --- secciones críticas y variables de condición --- */
void InitializeCriticalSection(CRITICAL_SECTION *cs);
static inline BOOL InitializeCriticalSectionAndSpinCount(CRITICAL_SECTION *cs, DWORD n) { (void)n; InitializeCriticalSection(cs); return TRUE; }
void DeleteCriticalSection(CRITICAL_SECTION *cs);
void EnterCriticalSection(CRITICAL_SECTION *cs);
BOOL TryEnterCriticalSection(CRITICAL_SECTION *cs);
void LeaveCriticalSection(CRITICAL_SECTION *cs);
static inline void InitializeConditionVariable(CONDITION_VARIABLE *c) { pthread_cond_init(&c->c, NULL); }
BOOL SleepConditionVariableCS(CONDITION_VARIABLE *c, CRITICAL_SECTION *cs, DWORD ms);
static inline void WakeConditionVariable(CONDITION_VARIABLE *c) { pthread_cond_signal(&c->c); }
static inline void WakeAllConditionVariable(CONDITION_VARIABLE *c) { pthread_cond_broadcast(&c->c); }

/* --- errores --- */
DWORD GetLastError(void);
void SetLastError(DWORD e);

/* --- hilos y objetos con espera (compat_win.c) --- */
HANDLE CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T pila, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, LPDWORD id);
HANDLE CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL inicial, LPCSTR nombre);
BOOL SetEvent(HANDLE h);
BOOL ResetEvent(HANDLE h);
HANDLE CreateSemaphoreA(LPSECURITY_ATTRIBUTES sa, LONG inicial, LONG maximo, LPCSTR nombre);
BOOL ReleaseSemaphore(HANDLE h, LONG n, LONG *antes);
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#define TIMER_ALL_ACCESS 0x1F0003
HANDLE CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES sa, LPCWSTR nombre, DWORD flags, DWORD acceso);
BOOL SetWaitableTimerEx(HANDLE h, const LARGE_INTEGER *due, LONG periodo, void *rutina, void *arg, void *razon, ULONG tolerancia);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
DWORD WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL todos, DWORD ms);
BOOL GetExitCodeThread(HANDLE h, LPDWORD code);
BOOL CloseHandle(HANDLE h);
DWORD GetCurrentThreadId(void);
HANDLE GetCurrentThread(void);
HANDLE GetCurrentProcess(void);
DWORD GetCurrentProcessId(void);
BOOL DuplicateHandle(HANDLE p1, HANDLE h, HANDLE p2, HANDLE *out, DWORD acceso, BOOL hereda, DWORD op);
static inline BOOL SetThreadPriority(HANDLE h, int p) { (void)h; (void)p; return TRUE; }
static inline BOOL SetPriorityClass(HANDLE h, DWORD c) { (void)h; (void)c; return TRUE; }
static inline BOOL SetThreadDescription(HANDLE h, LPCWSTR d) { (void)h; (void)d; return TRUE; }
__attribute__((noreturn)) void ExitProcess(UINT code);
__attribute__((noreturn)) void ExitThread(DWORD code);

/* Ahorro de energía de Windows 11 (sin equivalente: no hace nada) */
typedef struct { ULONG Version, ControlMask, StateMask; } PROCESS_POWER_THROTTLING_STATE;
#define PROCESS_POWER_THROTTLING_CURRENT_VERSION 1
#define PROCESS_POWER_THROTTLING_EXECUTION_SPEED 1
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 4
#define ProcessPowerThrottling 4
static inline BOOL SetProcessInformation(HANDLE h, int clase, void *info, DWORD tam) { (void)h; (void)clase; (void)info; (void)tam; return TRUE; }

/* --- sistema --- */
void GetSystemInfo(SYSTEM_INFO *si);
BOOL GlobalMemoryStatusEx(MEMORYSTATUSEX *m);
DWORD GetModuleFileNameA(HMODULE m, LPSTR buf, DWORD tam);   /* = carpeta de la app + "/ProjectBaseball" */
DWORD GetCurrentDirectoryA(DWORD tam, LPSTR buf);
HMODULE GetModuleHandleA(LPCSTR nombre);
HMODULE LoadLibraryA(LPCSTR ruta);
FARPROC GetProcAddress(HMODULE m, LPCSTR nombre);
BOOL FreeLibrary(HMODULE m);
int MessageBoxA(HWND w, LPCSTR texto, LPCSTR titulo, UINT tipo);
void OutputDebugStringA(LPCSTR s);
/* carpeta de datos de la app (la pone el arranque de Android antes de nada) */
void pb_android_set_dir(const char *dir);
const char *pb_android_dir(void);

/* --- archivos --- */
#define FILE_ATTRIBUTE_READONLY 0x1
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_NORMAL 0x80
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define FILE_SHARE_DELETE 4
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2
#define INVALID_SET_FILE_POINTER 0xFFFFFFFFu
#define MOVEFILE_REPLACE_EXISTING 1
#define MOVEFILE_COPY_ALLOWED 2
#define MOVEFILE_WRITE_THROUGH 8
typedef enum { GetFileExInfoStandard } GET_FILEEX_INFO_LEVELS;
typedef struct { DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow; } WIN32_FILE_ATTRIBUTE_DATA;
typedef struct { DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1; CHAR cFileName[MAX_PATH]; CHAR cAlternateFileName[14]; } WIN32_FIND_DATAA;
char *pb_ruta(const char *ruta, char *buf, size_t tam);   /* '\' -> '/' */
BOOL CreateDirectoryA(LPCSTR ruta, LPSECURITY_ATTRIBUTES sa);
BOOL RemoveDirectoryA(LPCSTR ruta);
BOOL DeleteFileA(LPCSTR ruta);
BOOL MoveFileExA(LPCSTR de, LPCSTR a, DWORD flags);
static inline BOOL MoveFileA(LPCSTR de, LPCSTR a) { return MoveFileExA(de, a, 0); }
BOOL CopyFileA(LPCSTR de, LPCSTR a, BOOL fallar_si_existe);
DWORD GetFileAttributesA(LPCSTR ruta);
BOOL GetFileAttributesExA(LPCSTR ruta, GET_FILEEX_INFO_LEVELS nivel, LPVOID info);
HANDLE FindFirstFileA(LPCSTR patron, WIN32_FIND_DATAA *d);
BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *d);
BOOL FindClose(HANDLE h);
HANDLE CreateFileA(LPCSTR ruta, DWORD acceso, DWORD compartir, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE plantilla);
BOOL ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD leidos, void *ov);
BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD escritos, void *ov);
DWORD SetFilePointer(HANDLE h, LONG dist, LONG *alto, DWORD desde);
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, LARGE_INTEGER *nueva, DWORD desde);
DWORD GetFileSize(HANDLE h, LPDWORD alto);
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER *t);
BOOL FlushFileBuffers(HANDLE h);
BOOL SetEndOfFile(HANDLE h);
FILE *pb_fopen(const char *ruta, const char *modo);
#define fopen pb_fopen
typedef struct { ULONG_PTR Internal, InternalHigh; union { struct { DWORD Offset, OffsetHigh; }; PVOID Pointer; }; HANDLE hEvent; } OVERLAPPED, *LPOVERLAPPED;

/* --- vigilancia de escrituras: no hay en Android (el port lo detecta y no la usa) --- */
#define WRITE_WATCH_FLAG_RESET 1
static inline UINT GetWriteWatch(DWORD f, PVOID base, SIZE_T tam, PVOID *dirs, ULONG_PTR *n, ULONG *gran) {
    (void)f; (void)base; (void)tam; (void)dirs; (void)n; (void)gran; return 1;
}

/* --- más tipos --- */
typedef int16_t SHORT;
typedef uint16_t USHORT;
typedef UINT_PTR WPARAM;
typedef LONG_PTR LPARAM, LRESULT;
typedef WORD ATOM;
typedef void *HBRUSH, *HICON, *HCURSOR, *HMENU, *HGDIOBJ, *HBITMAP, *HFONT;
typedef struct { LONG left, top, right, bottom; } RECT, *LPRECT;
typedef struct { LONG x, y; } POINT;

/* --- .ini (como GetPrivateProfileString de Windows; compat_win.c) --- */
DWORD GetPrivateProfileStringA(LPCSTR sec, LPCSTR clave, LPCSTR def, LPSTR out, DWORD tam, LPCSTR archivo);
UINT GetPrivateProfileIntA(LPCSTR sec, LPCSTR clave, INT def, LPCSTR archivo);
BOOL WritePrivateProfileStringA(LPCSTR sec, LPCSTR clave, LPCSTR valor, LPCSTR archivo);

/* --- texto --- */
#define CP_ACP 0
#define CP_UTF8 65001
int MultiByteToWideChar(UINT cp, DWORD f, LPCSTR s, int n, LPWSTR w, int nw);
int WideCharToMultiByte(UINT cp, DWORD f, LPCWSTR w, int nw, LPSTR s, int n, LPCSTR def, LPBOOL usado);
int MessageBoxW(HWND w, LPCWSTR texto, LPCWSTR titulo, UINT tipo);
static inline char *_strupr(char *s) { for (char *p = s; p && *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32); return s; }
static inline char *_strlwr(char *s) { for (char *p = s; p && *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32); return s; }
static inline HWND GetConsoleWindow(void) { return NULL; }

/* --- fechas del CRT de MSVC --- */
static inline int gmtime_s(struct tm *r, const time_t *t) { return gmtime_r(t, r) ? 0 : 1; }
static inline int localtime_s(struct tm *r, const time_t *t) { return localtime_r(t, r) ? 0 : 1; }
#define _mkgmtime timegm
#define _mktime64 mktime
#define _time64 time

/* --- ventanas y GDI: el port en Android dibuja con Vulkan/SDL; esto solo existe para que compile la
 *     ventana GDI de reserva de hle_display.c, que en Android no se abre nunca. --- */
#define WM_CLOSE 0x10
#define WM_DESTROY 0x2
#define WM_KEYDOWN 0x100
#define WM_PAINT 0xF
#define WM_QUIT 0x12
#define WM_SIZE 5
#define VK_ESCAPE 0x1B
#define SW_SHOW 5
#define SW_HIDE 0
#define PM_REMOVE 1
#define CS_HREDRAW 2
#define CS_VREDRAW 1
#define CS_OWNDC 0x20
#define WS_OVERLAPPEDWINDOW 0xCF0000
#define WS_VISIBLE 0x10000000
#define CW_USEDEFAULT ((int)0x80000000)
#define IDC_ARROW ((LPCSTR)32512)
#define DIB_RGB_COLORS 0
#define BI_RGB 0
#define SRCCOPY 0xCC0020
#define COLOR_WINDOW 5
typedef LRESULT (*WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef struct { UINT style; WNDPROC lpfnWndProc; int cbClsExtra, cbWndExtra; HINSTANCE hInstance; HICON hIcon; HCURSOR hCursor; HBRUSH hbrBackground; LPCSTR lpszMenuName, lpszClassName; } WNDCLASSA;
typedef struct { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; POINT pt; } MSG;
typedef struct { HDC hdc; BOOL fErase; RECT rcPaint; BOOL fRestore, fIncUpdate; BYTE rgbReserved[32]; } PAINTSTRUCT;
typedef struct { DWORD biSize; LONG biWidth, biHeight; WORD biPlanes, biBitCount; DWORD biCompression, biSizeImage; LONG biXPelsPerMeter, biYPelsPerMeter; DWORD biClrUsed, biClrImportant; } BITMAPINFOHEADER;
typedef struct { BYTE rgbBlue, rgbGreen, rgbRed, rgbReserved; } RGBQUAD;
typedef struct { BITMAPINFOHEADER bmiHeader; RGBQUAD bmiColors[1]; } BITMAPINFO;
static inline ATOM RegisterClassA(const WNDCLASSA *c) { (void)c; return 0; }
static inline HWND CreateWindowA(LPCSTR c, LPCSTR t, DWORD s, int x, int y, int w, int h, HWND p, HMENU m, HINSTANCE i, LPVOID l) {
    (void)c; (void)t; (void)s; (void)x; (void)y; (void)w; (void)h; (void)p; (void)m; (void)i; (void)l; return NULL; }
static inline HWND CreateWindowExA(DWORD e, LPCSTR c, LPCSTR t, DWORD s, int x, int y, int w, int h, HWND p, HMENU m, HINSTANCE i, LPVOID l) {
    (void)e; return CreateWindowA(c, t, s, x, y, w, h, p, m, i, l); }
static inline BOOL ShowWindow(HWND w, int c) { (void)w; (void)c; return FALSE; }
static inline BOOL UpdateWindow(HWND w) { (void)w; return FALSE; }
static inline BOOL DestroyWindow(HWND w) { (void)w; return FALSE; }
static inline BOOL PeekMessageA(MSG *m, HWND w, UINT a, UINT b, UINT r) { (void)m; (void)w; (void)a; (void)b; (void)r; return FALSE; }
static inline BOOL TranslateMessage(const MSG *m) { (void)m; return FALSE; }
static inline LRESULT DispatchMessageA(const MSG *m) { (void)m; return 0; }
static inline LRESULT DefWindowProcA(HWND w, UINT m, WPARAM a, LPARAM b) { (void)w; (void)m; (void)a; (void)b; return 0; }
static inline void PostQuitMessage(int c) { (void)c; }
static inline HDC GetDC(HWND w) { (void)w; return NULL; }
static inline int ReleaseDC(HWND w, HDC d) { (void)w; (void)d; return 0; }
static inline HDC BeginPaint(HWND w, PAINTSTRUCT *p) { (void)w; memset(p, 0, sizeof(*p)); return NULL; }
static inline BOOL EndPaint(HWND w, const PAINTSTRUCT *p) { (void)w; (void)p; return TRUE; }
static inline BOOL InvalidateRect(HWND w, const RECT *r, BOOL e) { (void)w; (void)r; (void)e; return TRUE; }
static inline BOOL GetClientRect(HWND w, RECT *r) { (void)w; memset(r, 0, sizeof(*r)); return FALSE; }
static inline BOOL AdjustWindowRect(RECT *r, DWORD s, BOOL m) { (void)r; (void)s; (void)m; return TRUE; }
static inline HCURSOR LoadCursorA(HINSTANCE i, LPCSTR n) { (void)i; (void)n; return NULL; }
static inline int StretchDIBits(HDC d, int x, int y, int w, int h, int sx, int sy, int sw, int sh, const void *b, const BITMAPINFO *bi, UINT u, DWORD op) {
    (void)d; (void)x; (void)y; (void)w; (void)h; (void)sx; (void)sy; (void)sw; (void)sh; (void)b; (void)bi; (void)u; (void)op; return 0; }
static inline int SetDIBitsToDevice(HDC d, int x, int y, DWORD w, DWORD h, int sx, int sy, UINT a, UINT n, const void *b, const BITMAPINFO *bi, UINT u) {
    (void)d; (void)x; (void)y; (void)w; (void)h; (void)sx; (void)sy; (void)a; (void)n; (void)b; (void)bi; (void)u; return 0; }
static inline BOOL SetWindowTextA(HWND w, LPCSTR t) { (void)w; (void)t; return FALSE; }
#define BLACK_BRUSH 4
#define WHITE_BRUSH 0
#define COLORONCOLOR 3
#define HALFTONE 4
static inline HGDIOBJ GetStockObject(int i) { (void)i; return NULL; }
static inline int FillRect(HDC d, const RECT *r, HBRUSH b) { (void)d; (void)r; (void)b; return 0; }
static inline int SetStretchBltMode(HDC d, int m) { (void)d; (void)m; return 0; }
/* pila del hilo actual (diagnóstico de cierres): en Android, sin pila */
static inline WORD RtlCaptureStackBackTrace(DWORD salta, DWORD n, PVOID *pila, DWORD *hash) { (void)salta; (void)n; (void)pila; if (hash) *hash = 0; return 0; }
#define RegisterClass RegisterClassA
#define CreateWindow CreateWindowA
#define CreateWindowEx CreateWindowExA
#define PeekMessage PeekMessageA
#define DispatchMessage DispatchMessageA
#define DefWindowProc DefWindowProcA
#define LoadCursor LoadCursorA
#define SetWindowText SetWindowTextA

#ifdef __cplusplus
}
#endif
#endif
