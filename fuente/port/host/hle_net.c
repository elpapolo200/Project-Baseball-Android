/* Red (SceNet, SceNetCtl, SceHttp, SceSsl, SceNp*) para el port de MLB 15.
 *
 * Por defecto se responde como una consola DESCONECTADA: Init/Term devuelven 0
 * y las operaciones reales (sockets, HTTP, PSN) devuelven error, así que el
 * juego sigue por la rama «sin conexión».
 *
 * MLB15_EN_LINEA=1 (multijugador en la misma red, docs/MULTIJUGADOR.md del port
 * de Android): red de verdad.
 *   - Sockets reales (Winsock / POSIX). El socket P2P de la Vita (tipo 6) va por UDP.
 *   - TODOS los nombres de servidor se resuelven a MLB15_SERVIDOR (la sala; por
 *     defecto 127.0.0.1): el juego nunca sale a internet.
 *   - sceHttp propio, en HTTP plano aunque la URL sea https (puertos 443/10051 -> 10050).
 *   - PSN «con sesión»: NpId con MLB15_JUGADOR, ticket de NpAuth falso, servicio en línea.
 *   - MLB15_NET_TRACE=1: cada socket, petición HTTP (cabeceras y cuerpo) y respuesta.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  ifdef _MSC_VER
#    pragma comment(lib, "ws2_32.lib")
#  endif
typedef SOCKET hsock;
#  define HSOCK_MALO INVALID_SOCKET
#  define cerrar_sock closesocket
static int ultimo_error(void) { return WSAGetLastError(); }
typedef CRITICAL_SECTION candado_t;
static void candado_ini(candado_t *c) { InitializeCriticalSection(c); }
static void candado_on(candado_t *c) { EnterCriticalSection(c); }
static void candado_off(candado_t *c) { LeaveCriticalSection(c); }
#else
#  include <errno.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <pthread.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <unistd.h>
typedef int hsock;
#  define HSOCK_MALO (-1)
#  define cerrar_sock close
static int ultimo_error(void) { return errno; }
typedef pthread_mutex_t candado_t;
static void candado_ini(candado_t *c) { pthread_mutex_init(c, NULL); }
static void candado_on(candado_t *c) { pthread_mutex_lock(c); }
static void candado_off(candado_t *c) { pthread_mutex_unlock(c); }
#endif

#include "vitarecomp/cpu.h"
#include "vitarecomp/mem.h"
#include "vitarecomp/dispatch.h"

#include "port.h"

#define NET_ERROR ((uint32_t)-1)

static int g_trace = -1;
static int trace_on(void) {
    if (g_trace < 0) g_trace = getenv("MLB15_NET_TRACE") ? 1 : 0;
    return g_trace;
}
#define TR(...) do { if (trace_on()) { printf(__VA_ARGS__); fflush(stdout); } } while (0)

#define ONCE(name) do { static int _o; if (!_o) { _o = 1; \
    printf("[net] %s(r0=0x%X r1=0x%X r2=0x%X r3=0x%X)\n", name, r0, r1, r2, r3); \
    fflush(stdout); } } while (0)

/* --- modo en línea ----------------------------------------------------------- */

static int g_linea = -1;
int net_en_linea(void) {
    if (g_linea < 0) {
        const char *e = getenv("MLB15_EN_LINEA");
        g_linea = e ? atoi(e) != 0 : 0;
        if (g_linea) printf("[net] modo en línea (sala en %s)\n", getenv("MLB15_SERVIDOR") ? getenv("MLB15_SERVIDOR") : "127.0.0.1");
    }
    return g_linea;
}

static candado_t g_cs;
static int g_cs_ok;
static void net_ini(void) {
    if (g_cs_ok) return;
    candado_ini(&g_cs);
#ifdef _WIN32
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
#endif
    g_cs_ok = 1;
}

static const char *jugador(void) {
    const char *e = getenv("MLB15_JUGADOR");
    return e && *e ? e : "Jugador";
}
static uint32_t ip_servidor(void) {   /* orden de red */
    const char *e = getenv("MLB15_SERVIDOR");
    struct in_addr a;
    if (!e || inet_pton(AF_INET, e, &a) != 1) inet_pton(AF_INET, "127.0.0.1", &a);
    return a.s_addr;
}

/* --- errores de SceNet (errno de BSD) -------------------------------------- */

#define SCE_NET_ERROR(e) (0x80410100u | (uint32_t)(e))
enum { B_EBADF = 9, B_EINVAL = 22, B_EAGAIN = 35, B_EINPROGRESS = 36, B_EALREADY = 37, B_EADDRINUSE = 48,
       B_EADDRNOTAVAIL = 49, B_ENETUNREACH = 51, B_ECONNABORTED = 53, B_ECONNRESET = 54, B_EISCONN = 56,
       B_ENOTCONN = 57, B_ETIMEDOUT = 60, B_ECONNREFUSED = 61, B_EHOSTUNREACH = 65, B_EMSGSIZE = 40 };

static uint32_t g_errno_guest;
static int bsd_errno(int e) {
#ifdef _WIN32
    switch (e) {
        case WSAEWOULDBLOCK: return B_EAGAIN;
        case WSAEINPROGRESS: return B_EINPROGRESS;
        case WSAEALREADY: return B_EALREADY;
        case WSAEADDRINUSE: return B_EADDRINUSE;
        case WSAEADDRNOTAVAIL: return B_EADDRNOTAVAIL;
        case WSAENETUNREACH: return B_ENETUNREACH;
        case WSAECONNABORTED: return B_ECONNABORTED;
        case WSAECONNRESET: return B_ECONNRESET;
        case WSAEISCONN: return B_EISCONN;
        case WSAENOTCONN: return B_ENOTCONN;
        case WSAETIMEDOUT: return B_ETIMEDOUT;
        case WSAECONNREFUSED: return B_ECONNREFUSED;
        case WSAEHOSTUNREACH: return B_EHOSTUNREACH;
        case WSAEMSGSIZE: return B_EMSGSIZE;
        case WSAENOTSOCK: case WSAEBADF: return B_EBADF;
        default: return B_EINVAL;
    }
#else
    switch (e) {
        case EAGAIN: return B_EAGAIN;
        case EINPROGRESS: return B_EINPROGRESS;
        case EALREADY: return B_EALREADY;
        case EADDRINUSE: return B_EADDRINUSE;
        case EADDRNOTAVAIL: return B_EADDRNOTAVAIL;
        case ENETUNREACH: return B_ENETUNREACH;
        case ECONNABORTED: return B_ECONNABORTED;
        case ECONNRESET: return B_ECONNRESET;
        case EISCONN: return B_EISCONN;
        case ENOTCONN: return B_ENOTCONN;
        case ETIMEDOUT: return B_ETIMEDOUT;
        case ECONNREFUSED: return B_ECONNREFUSED;
        case EHOSTUNREACH: return B_EHOSTUNREACH;
        case EMSGSIZE: return B_EMSGSIZE;
        case EBADF: return B_EBADF;
        default: return B_EINVAL;
    }
#endif
}
static uint32_t net_fallo(int bsd) {
    if (g_errno_guest) vita_write32(g_errno_guest, (uint32_t)bsd);
    return SCE_NET_ERROR(bsd);
}
static uint32_t net_fallo_sis(void) { return net_fallo(bsd_errno(ultimo_error())); }

/* --- SceNet --------------------------------------------------------------- */

void vita_hle_sceNetInit(void) {
    ONCE("sceNetInit");
    if (net_en_linea()) net_ini();
    r0 = 0;
}
void vita_hle_sceNetTerm(void) { r0 = 0; }

void vita_hle_sceNetErrnoLoc(void) {
    if (!g_errno_guest) g_errno_guest = port_alloc(4, 4, "net errno");
    r0 = g_errno_guest;
}

/* Tabla de sockets: id del juego = índice + 1. */
#define MAX_SOCK 64
typedef struct { hsock s; int tipo; int nbio; int usado; } Sock;
static Sock g_sock[MAX_SOCK];
static Sock *sock_de(uint32_t id) {
    if (id < 1 || id > MAX_SOCK || !g_sock[id - 1].usado) return NULL;
    return &g_sock[id - 1];
}

/* SceNetSockaddrIn: len(1) family(1) port(2, red) addr(4, red) vport(2) zero(6) */
static void addr_de_guest(uint32_t ga, struct sockaddr_in *sa) {
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    if (!ga) return;
    uint16_t port; uint32_t ad;
    memcpy(&port, vita_mem_ptr(ga + 2, 2), 2);
    memcpy(&ad, vita_mem_ptr(ga + 4, 4), 4);
    sa->sin_port = port;
    sa->sin_addr.s_addr = ad;
}
static void addr_a_guest(const struct sockaddr_in *sa, uint32_t ga, uint32_t galen) {
    if (!ga) return;
    uint8_t b[16] = { 16, 2 };
    memcpy(b + 2, &sa->sin_port, 2);
    memcpy(b + 4, &sa->sin_addr.s_addr, 4);
    memcpy(vita_mem_ptr(ga, 16), b, 16);
    if (galen) vita_write32(galen, 16);
}
static const char *addr_txt(const struct sockaddr_in *sa, char *b) {
    char ip[32]; inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
    sprintf(b, "%s:%u", ip, ntohs(sa->sin_port));
    return b;
}

void vita_hle_sceNetSocket(void) {   /* (name, domain, type, protocol) */
    if (!net_en_linea()) { r0 = NET_ERROR; return; }
    net_ini();
    int tipo = (int)r2;
    int st = (tipo == 1 || tipo == 10) ? SOCK_STREAM : SOCK_DGRAM;   /* 6 = DGRAM_P2P -> UDP */
    hsock s = socket(AF_INET, st, st == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP);
    if (s == HSOCK_MALO) { r0 = net_fallo_sis(); return; }
    candado_on(&g_cs);
    int i; for (i = 0; i < MAX_SOCK && g_sock[i].usado; i++) {}
    if (i == MAX_SOCK) { candado_off(&g_cs); cerrar_sock(s); r0 = net_fallo(B_EINVAL); return; }
    g_sock[i].s = s; g_sock[i].tipo = tipo; g_sock[i].nbio = 0; g_sock[i].usado = 1;
    candado_off(&g_cs);
    char nombre[32] = ""; if (r0) port_guest_str(r0, nombre, sizeof(nombre));
    TR("[net] socket '%s' tipo %d -> %d\n", nombre, tipo, i + 1);
    r0 = (uint32_t)(i + 1);
}
void vita_hle_sceNetSocketClose(void) {
    Sock *k = net_en_linea() ? sock_de(r0) : NULL;
    if (!k) { r0 = net_en_linea() ? net_fallo(B_EBADF) : NET_ERROR; return; }
    TR("[net] cerrar %u\n", r0);
    cerrar_sock(k->s); k->usado = 0;
    r0 = 0;
}
void vita_hle_sceNetSocketAbort(void) {
    Sock *k = net_en_linea() ? sock_de(r0) : NULL;
    if (!k) { r0 = net_en_linea() ? net_fallo(B_EBADF) : NET_ERROR; return; }
#ifdef _WIN32
    shutdown(k->s, SD_BOTH);
#else
    shutdown(k->s, SHUT_RDWR);
#endif
    r0 = 0;
}
void vita_hle_sceNetBind(void) {   /* (s, addr, len) */
    Sock *k = net_en_linea() ? sock_de(r0) : NULL;
    if (!k) { r0 = net_en_linea() ? net_fallo(B_EBADF) : NET_ERROR; return; }
    struct sockaddr_in sa; addr_de_guest(r1, &sa); char t[48];
    int rc = bind(k->s, (struct sockaddr *)&sa, sizeof(sa));
    TR("[net] bind %u %s -> %d\n", r0, addr_txt(&sa, t), rc);
    r0 = rc == 0 ? 0 : net_fallo_sis();
}
void vita_hle_sceNetConnect(void) {   /* (s, addr, len) */
    Sock *k = net_en_linea() ? sock_de(r0) : NULL;
    if (!k) { r0 = net_en_linea() ? net_fallo(B_EBADF) : NET_ERROR; return; }
    struct sockaddr_in sa; addr_de_guest(r1, &sa); char t[48];
    int rc = connect(k->s, (struct sockaddr *)&sa, sizeof(sa));
    uint32_t res = rc == 0 ? 0 : net_fallo_sis();
    TR("[net] connect %u %s -> 0x%X\n", r0, addr_txt(&sa, t), res);
    r0 = res;
}
void vita_hle_sceNetListen(void) {
    Sock *k = net_en_linea() ? sock_de(r0) : NULL;
    if (!k) { r0 = net_en_linea() ? net_fallo(B_EBADF) : NET_ERROR; return; }
    int rc = listen(k->s, (int)(r1 ? r1 : 8));
    TR("[net] listen %u -> %d\n", r0, rc);
    r0 = rc == 0 ? 0 : net_fallo_sis();
}
void vita_hle_sceNetAccept(void) {   /* (s, addr*, addrlen*) */
    Sock *k = net_en_linea() ? sock_de(r0) : NULL;
    if (!k) { r0 = net_en_linea() ? net_fallo(B_EBADF) : NET_ERROR; return; }
    struct sockaddr_in sa; socklen_t l = sizeof(sa);
    uint32_t ga = r1, gl = r2;
    hsock s = accept(k->s, (struct sockaddr *)&sa, &l);
    if (s == HSOCK_MALO) { r0 = net_fallo_sis(); return; }
    candado_on(&g_cs);
    int i; for (i = 0; i < MAX_SOCK && g_sock[i].usado; i++) {}
    if (i == MAX_SOCK) { candado_off(&g_cs); cerrar_sock(s); r0 = net_fallo(B_EINVAL); return; }
    g_sock[i].s = s; g_sock[i].tipo = 1; g_sock[i].nbio = 0; g_sock[i].usado = 1;
    candado_off(&g_cs);
    addr_a_guest(&sa, ga, gl);
    char t[48]; TR("[net] accept -> %d de %s\n", i + 1, addr_txt(&sa, t));
    r0 = (uint32_t)(i + 1);
}

static int flags_host(uint32_t f) {
    int h = 0;
    if (f & 0x02) h |= MSG_PEEK;
#ifndef _WIN32
    if (f & 0x80) h |= MSG_DONTWAIT;
#endif
    return h;
}
static void volcar(const char *que, uint32_t id, const uint8_t *p, int n) {
    if (!trace_on() || n <= 0) return;
    printf("[net] %s %u (%d B):", que, id, n);
    for (int i = 0; i < n && i < 48; i++) printf(" %02X", p[i]);
    printf(" |");
    for (int i = 0; i < n && i < 160; i++) putchar(p[i] >= 32 && p[i] < 127 ? p[i] : '.');
    printf("|\n"); fflush(stdout);
}
void vita_hle_sceNetSend(void) {   /* (s, buf, len, flags) */
    Sock *k = net_en_linea() ? sock_de(r0) : NULL;
    if (!k) { r0 = net_en_linea() ? net_fallo(B_EBADF) : NET_ERROR; return; }
    const uint8_t *p = (const uint8_t *)vita_mem_ptr(r1, r2 ? r2 : 1);
    int n = send(k->s, (const char *)p, (int)r2, flags_host(r3));
    volcar("send", r0, p, n);
    r0 = n >= 0 ? (uint32_t)n : net_fallo_sis();
}
void vita_hle_sceNetSendto(void) {   /* (s, buf, len, flags, to, tolen) */
    Sock *k = net_en_linea() ? sock_de(r0) : NULL;
    if (!k) { r0 = net_en_linea() ? net_fallo(B_EBADF) : NET_ERROR; return; }
    uint32_t gto = vita_read32(sp);
    const uint8_t *p = (const uint8_t *)vita_mem_ptr(r1, r2 ? r2 : 1);
    int n;
    if (gto) { struct sockaddr_in sa; addr_de_guest(gto, &sa); n = sendto(k->s, (const char *)p, (int)r2, flags_host(r3), (struct sockaddr *)&sa, sizeof(sa)); }
    else n = send(k->s, (const char *)p, (int)r2, flags_host(r3));
    volcar("sendto", r0, p, n);
    r0 = n >= 0 ? (uint32_t)n : net_fallo_sis();
}
void vita_hle_sceNetRecv(void) {   /* (s, buf, len, flags) */
    Sock *k = net_en_linea() ? sock_de(r0) : NULL;
    if (!k) { r0 = net_en_linea() ? net_fallo(B_EBADF) : NET_ERROR; return; }
    uint32_t id = r0;
    uint8_t *p = (uint8_t *)vita_mem_ptr(r1, r2 ? r2 : 1);
    int n = recv(k->s, (char *)p, (int)r2, flags_host(r3));
    if (n < 0) { r0 = net_fallo_sis(); return; }
    volcar("recv", id, p, n);
    r0 = (uint32_t)n;
}
void vita_hle_sceNetRecvfrom(void) {   /* (s, buf, len, flags, from, fromlen) */
    Sock *k = net_en_linea() ? sock_de(r0) : NULL;
    if (!k) { r0 = net_en_linea() ? net_fallo(B_EBADF) : NET_ERROR; return; }
    uint32_t id = r0, gfrom = vita_read32(sp), glen = vita_read32(sp + 4);
    uint8_t *p = (uint8_t *)vita_mem_ptr(r1, r2 ? r2 : 1);
    struct sockaddr_in sa; socklen_t l = sizeof(sa); memset(&sa, 0, sizeof(sa));
    int n = recvfrom(k->s, (char *)p, (int)r2, flags_host(r3), (struct sockaddr *)&sa, &l);
    if (n < 0) { r0 = net_fallo_sis(); return; }
    addr_a_guest(&sa, gfrom, glen);
    volcar("recvfrom", id, p, n);
    r0 = (uint32_t)n;
}
void vita_hle_sceNetSetsockopt(void) {   /* (s, level, optname, optval, optlen) */
    Sock *k = net_en_linea() ? sock_de(r0) : NULL;
    if (!k) { r0 = net_en_linea() ? net_fallo(B_EBADF) : NET_ERROR; return; }
    uint32_t nivel = r1, op = r2, val = r3 ? vita_read32(r3) : 0;
    int rc = 0;
    if (nivel == 0xFFFF && op == 0x1100) {   /* SO_NBIO */
        k->nbio = val != 0;
#ifdef _WIN32
        u_long m = val != 0; rc = ioctlsocket(k->s, FIONBIO, &m);
#else
        int fl = fcntl(k->s, F_GETFL, 0); rc = fcntl(k->s, F_SETFL, val ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK)) < 0 ? -1 : 0;
#endif
    } else if (nivel == 0xFFFF && (op == 0x1005 || op == 0x1006)) {   /* SO_SNDTIMEO / SO_RCVTIMEO (µs) */
#ifdef _WIN32
        DWORD ms = val / 1000; rc = setsockopt(k->s, SOL_SOCKET, op == 0x1005 ? SO_SNDTIMEO : SO_RCVTIMEO, (const char *)&ms, sizeof(ms));
#else
        struct timeval tv = { (time_t)(val / 1000000), (suseconds_t)(val % 1000000) };
        rc = setsockopt(k->s, SOL_SOCKET, op == 0x1005 ? SO_SNDTIMEO : SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    } else if (nivel == 0xFFFF && op == 0x20) { int v = val != 0; rc = setsockopt(k->s, SOL_SOCKET, SO_BROADCAST, (const char *)&v, sizeof(v)); }
    else if (nivel == 0xFFFF && op == 0x4) { int v = val != 0; rc = setsockopt(k->s, SOL_SOCKET, SO_REUSEADDR, (const char *)&v, sizeof(v)); }
    else if (nivel == 0xFFFF && (op == 0x1001 || op == 0x1002)) { int v = (int)val; rc = setsockopt(k->s, SOL_SOCKET, op == 0x1001 ? SO_SNDBUF : SO_RCVBUF, (const char *)&v, sizeof(v)); }
    else if (nivel == 6 && op == 1) { int v = val != 0; rc = setsockopt(k->s, IPPROTO_TCP, TCP_NODELAY, (const char *)&v, sizeof(v)); }
    TR("[net] setsockopt %u nivel 0x%X op 0x%X val %u -> %d\n", r0, nivel, op, val, rc);
    r0 = 0;
}

/* Resolver: todo nombre va a la sala. */
void vita_hle_sceNetResolverCreate(void) { r0 = net_en_linea() ? 1 : NET_ERROR; }
void vita_hle_sceNetResolverDestroy(void) { r0 = net_en_linea() ? 0 : NET_ERROR; }
void vita_hle_sceNetResolverStartNtoa(void) {   /* (rid, hostname, SceNetInAddr*, timeout, retry, flags) */
    if (!net_en_linea()) { r0 = NET_ERROR; return; }
    char host[128] = ""; port_guest_str(r1, host, sizeof(host));
    uint32_t a = ip_servidor();
    if (r2) memcpy(vita_mem_ptr(r2, 4), &a, 4);
    struct in_addr ia; ia.s_addr = a; char ip[32]; inet_ntop(AF_INET, &ia, ip, sizeof(ip));
    TR("[net] resolver '%s' -> %s (sala)\n", host, ip);
    r0 = 0;
}

void vita_hle_sceNetHtons(void) { r0 = ((r0 & 0xFFu) << 8) | ((r0 >> 8) & 0xFFu); }
void vita_hle_sceNetNtohs(void) { vita_hle_sceNetHtons(); }
void vita_hle_sceNetHtonl(void) {
    r0 = ((r0 & 0x000000FFu) << 24) | ((r0 & 0x0000FF00u) << 8) |
         ((r0 & 0x00FF0000u) >> 8)  | ((r0 & 0xFF000000u) >> 24);
}
void vita_hle_sceNetNtohl(void) { vita_hle_sceNetHtonl(); }

/* --- SceNetCtl ------------------------------------------------------------ */

static uint32_t ip_local(void) {   /* la IP de este equipo hacia la sala (orden de red) */
    net_ini();
    {   /* MLB15_IP_LOCAL: para probar dos copias en el mismo PC (127.0.0.1 y 127.0.0.2) */
        const char *e = getenv("MLB15_IP_LOCAL"); struct in_addr a;
        if (e && inet_pton(AF_INET, e, &a) == 1) return a.s_addr;
    }
    hsock s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET; sa.sin_port = htons(10050); sa.sin_addr.s_addr = ip_servidor();
    /* Anfitrión (sala en este mismo equipo, 127.x): la IP de la red local es la de la ruta por defecto.
     * connect() en UDP no envía nada: solo pregunta al sistema qué interfaz usaría. */
    if ((ntohl(sa.sin_addr.s_addr) >> 24) == 127) inet_pton(AF_INET, "8.8.8.8", &sa.sin_addr);
    uint32_t r = htonl(0x7F000001);
    if (s != HSOCK_MALO) {
        if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
            struct sockaddr_in me; socklen_t l = sizeof(me);
            if (getsockname(s, (struct sockaddr *)&me, &l) == 0) r = me.sin_addr.s_addr;
        }
        cerrar_sock(s);
    }
    return r;
}
static void mac_falsa(uint8_t m[6]) {   /* distinta por jugador (dos copias en el mismo PC) */
    uint32_t h = 2166136261u;
    for (const char *p = jugador(); *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    m[0] = 0x02; m[1] = 0x50; m[2] = (uint8_t)(h >> 24); m[3] = (uint8_t)(h >> 16); m[4] = (uint8_t)(h >> 8); m[5] = (uint8_t)h;
}

void vita_hle_sceNetCtlInit(void) { ONCE("sceNetCtlInit"); r0 = 0; }
void vita_hle_sceNetCtlTerm(void) { r0 = 0; }
void vita_hle_sceNetCtlCheckCallback(void) { r0 = 0; }

void vita_hle_sceNetCtlInetGetState(void) {
    /* r0 = int* state; 0 = desconectado, 3 = IP obtenida */
    uint32_t st = net_en_linea() ? 3u : 0u;
    if (vita_mem_ptr(r0, 4)) vita_write32(r0, st);
    ONCE(net_en_linea() ? "sceNetCtlInetGetState -> conectado" : "sceNetCtlInetGetState -> desconectado");
    r0 = 0;
}

void vita_hle_sceNetCtlInetGetInfo(void) {   /* (code, SceNetCtlInfo*) */
    if (!net_en_linea() || !r1) { r0 = NET_ERROR; return; }
    uint32_t code = r0, p = r1;
    uint8_t *d = (uint8_t *)vita_mem_ptr(p, 64);
    memset(d, 0, 64);
    struct in_addr ia; char ip[32];
    switch (code) {   /* SCE_NETCTL_INFO_GET_*: 1 CNF_NAME, 2 DEVICE, 3 ETHER_ADDR, ... 15 IP_ADDRESS */
        case 1: strcpy((char *)d, "ProjectBaseball"); break;                 /* CNF_NAME */
        case 2: vita_write32(p, 0); break;                                   /* DEVICE: wifi */
        case 3: mac_falsa(d); break;                                         /* ETHER_ADDR */
        case 4: vita_write32(p, 1500); break;                                /* MTU */
        case 5: vita_write32(p, 1); break;                                   /* LINK */
        case 7: strcpy((char *)d, "ProjectBaseball"); break;                 /* SSID */
        case 9: vita_write32(p, (uint32_t)-40); break;                       /* RSSI dBm */
        case 10: vita_write32(p, 100); break;                                /* RSSI % */
        case 15: case 17: case 18:                                           /* IP, ruta, DNS */
            ia.s_addr = code == 15 ? ip_local() : ip_servidor();
            inet_ntop(AF_INET, &ia, ip, sizeof(ip)); strcpy((char *)d, ip); break;
        case 16: strcpy((char *)d, "255.255.255.0"); break;                  /* NETMASK */
        default: break;
    }
    TR("[net] NetCtlInetGetInfo código %u\n", code);
    r0 = 0;
}
void vita_hle_sceNetCtlGetNatInfo(void) {   /* SceNetCtlNatInfo { size, stun_status, nat_type, mapped_addr } */
    if (!net_en_linea() || !r0) { r0 = NET_ERROR; return; }
    vita_write32(r0 + 4, 1);
    vita_write32(r0 + 8, 1);
    uint32_t a = ip_local(); memcpy(vita_mem_ptr(r0 + 12, 4), &a, 4);
    r0 = 0;
}

void vita_hle_sceNetCtlInetRegisterCallback(void) {
    /* r0 = funcion, r1 = arg, r2 = int* id */
    if (r2 && vita_mem_ptr(r2, 4)) vita_write32(r2, 1);
    r0 = 0;
}
void vita_hle_sceNetCtlInetUnregisterCallback(void) { r0 = 0; }

/* --- llamadas al juego (callbacks de PSN) ------------------------------------ */

static void llamar_guest(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
    if (!fn) return;
    uint32_t sv[15] = { r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12, sp, lr };
    int fl[4] = { flag_n, flag_z, flag_c, flag_v };
    sp = (sp - 0x40) & ~7u;
    r0 = a0; r1 = a1; r2 = a2; lr = 0;
    vita_dispatch(0, fn);
    r0 = sv[0]; r1 = sv[1]; r2 = sv[2]; r3 = sv[3]; r4 = sv[4]; r5 = sv[5]; r6 = sv[6]; r7 = sv[7];
    r8 = sv[8]; r9 = sv[9]; r10 = sv[10]; r11 = sv[11]; r12 = sv[12]; sp = sv[13]; lr = sv[14];
    flag_n = fl[0]; flag_z = fl[1]; flag_c = fl[2]; flag_v = fl[3];
}

/* --- SceHttp -------------------------------------------------------------- */

#define MAX_HTTP 32
#define HDR_GUEST 8192
typedef struct {
    int usado, tipo;               /* 1 plantilla, 2 conexión, 3 petición */
    int padre;
    int metodo;
    char url[512];
    char cab[2048]; int ncab;      /* cabeceras añadidas ("Nombre: valor\r\n") */
    int estado;
    uint8_t *resp; int nresp, leido;
    char *rcab; int nrcab;
    uint32_t gcab;                 /* copia de las cabeceras en memoria guest */
} Http;
static Http g_http[MAX_HTTP];

static int http_nuevo(int tipo, int padre) {
    candado_on(&g_cs);
    int i; for (i = 0; i < MAX_HTTP && g_http[i].usado; i++) {}
    if (i < MAX_HTTP) {
        uint32_t gc = g_http[i].gcab;
        memset(&g_http[i], 0, sizeof(Http));
        g_http[i].gcab = gc; g_http[i].usado = 1; g_http[i].tipo = tipo; g_http[i].padre = padre;
    }
    candado_off(&g_cs);
    return i < MAX_HTTP ? i + 1 : -1;
}
static Http *http_de(uint32_t id) { return id >= 1 && id <= MAX_HTTP && g_http[id - 1].usado ? &g_http[id - 1] : NULL; }
static void http_borrar(uint32_t id) {
    Http *h = http_de(id); if (!h) return;
    free(h->resp); free(h->rcab); h->resp = NULL; h->rcab = NULL; h->usado = 0;
}
#define HTTP_ERR 0x80431100u

void vita_hle_sceHttpInit(void) { if (net_en_linea()) net_ini(); r0 = 0; }
void vita_hle_sceHttpTerm(void) { r0 = 0; }
void vita_hle_sceHttpCreateTemplate(void) {   /* (userAgent, httpVer, autoProxy) */
    if (!net_en_linea()) { r0 = NET_ERROR; return; }
    int id = http_nuevo(1, 0);
    Http *h = http_de((uint32_t)id);
    if (h && r0) { char ua[200]; port_guest_str(r0, ua, sizeof(ua)); h->ncab = snprintf(h->cab, sizeof(h->cab), "User-Agent: %s\r\n", ua); }
    r0 = id > 0 ? (uint32_t)id : HTTP_ERR;
}
void vita_hle_sceHttpDeleteTemplate(void) { if (net_en_linea()) http_borrar(r0); r0 = net_en_linea() ? 0 : NET_ERROR; }
void vita_hle_sceHttpCreateConnectionWithURL(void) {   /* (tmpl, url, keepalive) */
    if (!net_en_linea()) { r0 = NET_ERROR; return; }
    int id = http_nuevo(2, (int)r0);
    Http *h = http_de((uint32_t)id);
    if (h) port_guest_str(r1, h->url, sizeof(h->url));
    r0 = id > 0 ? (uint32_t)id : HTTP_ERR;
}
void vita_hle_sceHttpDeleteConnection(void) { if (net_en_linea()) http_borrar(r0); r0 = net_en_linea() ? 0 : NET_ERROR; }
void vita_hle_sceHttpCreateRequestWithURL(void) {   /* (conn, method, url, contentLength u64 en la pila) */
    if (!net_en_linea()) { r0 = NET_ERROR; return; }
    int id = http_nuevo(3, (int)r0);
    Http *h = http_de((uint32_t)id);
    if (h) { h->metodo = (int)r1; port_guest_str(r2, h->url, sizeof(h->url)); }
    r0 = id > 0 ? (uint32_t)id : HTTP_ERR;
}
void vita_hle_sceHttpDeleteRequest(void) { if (net_en_linea()) http_borrar(r0); r0 = net_en_linea() ? 0 : NET_ERROR; }
void vita_hle_sceHttpAddRequestHeader(void) {   /* (id, name, value, mode) */
    Http *h = net_en_linea() ? http_de(r0) : NULL;
    if (!h) { r0 = net_en_linea() ? HTTP_ERR : NET_ERROR; return; }
    char n[128], v[1024]; port_guest_str(r1, n, sizeof(n)); port_guest_str(r2, v, sizeof(v));
    if (h->ncab < (int)sizeof(h->cab) - 1)
        h->ncab += snprintf(h->cab + h->ncab, sizeof(h->cab) - (size_t)h->ncab, "%s: %s\r\n", n, v);
    r0 = 0;
}

static const char *k_metodo[] = { "GET", "POST", "HEAD", "OPTIONS", "PUT", "DELETE", "TRACE", "CONNECT" };

/* Hace la petición entera (HTTP/1.0, la respuesta termina al cerrar). */
static int http_hacer(Http *h, const uint8_t *cuerpo, uint32_t ncuerpo) {
    char host[256] = "", ruta[1024] = "/";
    int puerto = 80;
    const char *u = h->url;
    int https = strncmp(u, "https://", 8) == 0;
    if (https) { u += 8; puerto = 443; } else if (strncmp(u, "http://", 7) == 0) u += 7;
    const char *bar = strchr(u, '/');
    size_t lh = bar ? (size_t)(bar - u) : strlen(u);
    if (lh >= sizeof(host)) lh = sizeof(host) - 1;
    memcpy(host, u, lh); host[lh] = 0;
    if (bar) snprintf(ruta, sizeof(ruta), "%s", bar);
    char *dp = strchr(host, ':'); if (dp) { puerto = atoi(dp + 1); *dp = 0; }
    if (puerto == 443 || puerto == 10051) puerto = 10050;   /* la sala habla HTTP plano */

    /* cabeceras: plantilla + conexión + petición */
    char cab[6144]; int nc = 0;
    Http *pc = http_de((uint32_t)h->padre), *pt = pc ? http_de((uint32_t)pc->padre) : NULL;
    if (pt) nc += snprintf(cab + nc, sizeof(cab) - (size_t)nc, "%s", pt->cab);
    if (pc) nc += snprintf(cab + nc, sizeof(cab) - (size_t)nc, "%s", pc->cab);
    nc += snprintf(cab + nc, sizeof(cab) - (size_t)nc, "%s", h->cab);

    const char *met = h->metodo >= 0 && h->metodo < 8 ? k_metodo[h->metodo] : "GET";
    char pet[8192];
    int np = snprintf(pet, sizeof(pet), "%s %s HTTP/1.0\r\nHost: %s\r\n%sContent-Length: %u\r\nConnection: close\r\n\r\n",
                      met, ruta, host, cab, ncuerpo);
    if (trace_on()) {
        printf("[http] >>> %s %s (%s)\n%s", met, h->url, https ? "https -> http" : "http", cab);
        if (ncuerpo) { printf("[http] cuerpo (%u B): ", ncuerpo); fwrite(cuerpo, 1, ncuerpo < 2048 ? ncuerpo : 2048, stdout); printf("\n"); }
        fflush(stdout);
    }
    hsock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == HSOCK_MALO) return -1;
    struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET; sa.sin_port = htons((uint16_t)puerto); sa.sin_addr.s_addr = ip_servidor();
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) { TR("[http] sin conexión con la sala (puerto %d)\n", puerto); cerrar_sock(s); return -1; }
    send(s, pet, np, 0);
    if (ncuerpo) send(s, (const char *)cuerpo, (int)ncuerpo, 0);
    int cap = 65536, n = 0; uint8_t *buf = (uint8_t *)malloc((size_t)cap);
    for (;;) {
        if (n + 16384 > cap) { cap *= 2; buf = (uint8_t *)realloc(buf, (size_t)cap); }
        int k = recv(s, (char *)buf + n, cap - n, 0);
        if (k <= 0) break;
        n += k;
    }
    cerrar_sock(s);
    /* separar estado, cabeceras y cuerpo */
    uint8_t *fin = NULL;
    for (int i = 0; i + 3 < n; i++) if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') { fin = buf + i; break; }
    if (!fin) { free(buf); return -1; }
    h->estado = 0;
    sscanf((const char *)buf, "HTTP/%*s %d", &h->estado);
    const char *l1 = strstr((const char *)buf, "\r\n");
    int ncab = (int)(fin - (const uint8_t *)l1) - 2 + 4;   /* cabeceras sin la línea de estado, con \r\n\r\n */
    if (ncab < 0) ncab = 0;
    h->rcab = (char *)malloc((size_t)ncab + 1);
    memcpy(h->rcab, l1 + 2, (size_t)ncab); h->rcab[ncab] = 0; h->nrcab = ncab;
    h->nresp = (int)(n - (fin + 4 - buf));
    h->resp = (uint8_t *)malloc((size_t)h->nresp + 1);
    memcpy(h->resp, fin + 4, (size_t)h->nresp); h->resp[h->nresp] = 0;
    h->leido = 0;
    free(buf);
    if (trace_on()) {
        printf("[http] <<< %d (%d B)\n", h->estado, h->nresp);
        fwrite(h->resp, 1, h->nresp < 2048 ? (size_t)h->nresp : 2048, stdout); printf("\n"); fflush(stdout);
    }
    return 0;
}
void vita_hle_sceHttpSendRequest(void) {   /* (req, postData, size) */
    Http *h = net_en_linea() ? http_de(r0) : NULL;
    if (!h) { r0 = net_en_linea() ? HTTP_ERR : NET_ERROR; return; }
    const uint8_t *c = r1 && r2 ? (const uint8_t *)vita_mem_ptr(r1, r2) : NULL;
    r0 = http_hacer(h, c, c ? r2 : 0) == 0 ? 0 : 0x80431063u;   /* error de red */
}
void vita_hle_sceHttpReadData(void) {   /* (req, data, size) */
    Http *h = net_en_linea() ? http_de(r0) : NULL;
    if (!h) { r0 = net_en_linea() ? HTTP_ERR : NET_ERROR; return; }
    int n = h->nresp - h->leido; if (n > (int)r2) n = (int)r2; if (n < 0) n = 0;
    if (n) { memcpy(vita_mem_ptr(r1, (uint32_t)n), h->resp + h->leido, (size_t)n); h->leido += n; }
    r0 = (uint32_t)n;
}
void vita_hle_sceHttpGetStatusCode(void) {   /* (req, int*) */
    Http *h = net_en_linea() ? http_de(r0) : NULL;
    if (!h) { r0 = net_en_linea() ? HTTP_ERR : NET_ERROR; return; }
    if (r1) vita_write32(r1, (uint32_t)h->estado);
    r0 = 0;
}
void vita_hle_sceHttpGetResponseContentLength(void) {   /* (req, uint64_t*) */
    Http *h = net_en_linea() ? http_de(r0) : NULL;
    if (!h) { r0 = net_en_linea() ? HTTP_ERR : NET_ERROR; return; }
    if (r1) { vita_write32(r1, (uint32_t)h->nresp); vita_write32(r1 + 4, 0); }
    r0 = 0;
}
void vita_hle_sceHttpGetAllResponseHeaders(void) {   /* (req, char** header, uint* size) */
    Http *h = net_en_linea() ? http_de(r0) : NULL;
    if (!h) { r0 = net_en_linea() ? HTTP_ERR : NET_ERROR; return; }
    if (!h->gcab) h->gcab = port_alloc(HDR_GUEST, 4, "http cabeceras");
    int n = h->nrcab < HDR_GUEST - 1 ? h->nrcab : HDR_GUEST - 1;
    if (n > 0) memcpy(vita_mem_ptr(h->gcab, (uint32_t)n), h->rcab, (size_t)n);
    vita_write8(h->gcab + (uint32_t)n, 0);
    if (r1) vita_write32(r1, h->gcab);
    if (r2) vita_write32(r2, (uint32_t)n);
    r0 = 0;
}
void vita_hle_sceHttpParseResponseHeader(void) {   /* (header, headerLen, fieldStr, const char** value, uint* len) */
    if (!net_en_linea()) { r0 = NET_ERROR; return; }
    uint32_t gh = r0, nh = r1, gv = r3, gl = vita_read32(sp);
    char campo[128]; port_guest_str(r2, campo, sizeof(campo));
    size_t lc = strlen(campo);
    const char *p = (const char *)vita_mem_ptr(gh, nh ? nh : 1);
    for (uint32_t i = 0; i + lc < nh; ) {
        int igual = i == 0 || p[i - 1] == '\n';
        for (size_t c = 0; igual && c < lc; c++) {
            char a = p[i + c], b = campo[c];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            igual = a == b;
        }
        if (igual && p[i + lc] == ':') {
            uint32_t v = i + (uint32_t)lc + 1;
            while (v < nh && p[v] == ' ') v++;
            uint32_t f = v; while (f < nh && p[f] != '\r' && p[f] != '\n') f++;
            if (gv) vita_write32(gv, gh + v);
            if (gl) vita_write32(gl, f - v);
            r0 = (uint32_t)(f - i);
            return;
        }
        while (i < nh && p[i] != '\n') i++;
        i++;
    }
    r0 = 0x80431081u;   /* no encontrada */
}
void vita_hle_sceHttpSetCookieEnabled(void) { r0 = net_en_linea() ? 0 : NET_ERROR; }
void vita_hle_sceHttpSetCookieRecvCallback(void) { r0 = net_en_linea() ? 0 : NET_ERROR; }
void vita_hle_sceHttpGetCookie(void) {   /* (url, cookie, required*, prepare, secure) */
    if (!net_en_linea()) { r0 = NET_ERROR; return; }
    if (r2) vita_write32(r2, 0);
    if (r1) vita_write8(r1, 0);
    r0 = 0;
}
void vita_hle_sceHttpsDisableOption(void) { r0 = net_en_linea() ? 0 : NET_ERROR; }

/* --- SceSsl --------------------------------------------------------------- */

void vita_hle_sceSslInit(void) { r0 = 0; }
void vita_hle_sceSslTerm(void) { r0 = 0; }

/* --- SceNp* --------------------------------------------------------------- */
/* Sin MLB15_EN_LINEA: Init/Term devuelven 0 y lo que requiere PSN, error. */

static uint32_t g_np_estado_cb, g_np_estado_arg; static int g_np_estado_dado;
static struct { uint32_t cb, arg; int pendiente; } g_auth[8];

static void escribir_npid(uint32_t p) {   /* SceNpId: SceNpOnlineId {char data[16]; char term; char dummy[3]} + opt[8] + reserved[8] */
    uint8_t *d = (uint8_t *)vita_mem_ptr(p, 36);
    memset(d, 0, 36);
    strncpy((char *)d, jugador(), 16);
}

void vita_hle_sceNpInit(void) { ONCE(net_en_linea() ? "sceNpInit (en línea)" : "sceNpInit (offline)"); r0 = 0; }
void vita_hle_sceNpTerm(void) { r0 = 0; }
void vita_hle_sceNpCheckCallback(void) {
    if (net_en_linea()) {
        if (g_np_estado_cb && !g_np_estado_dado) {
            g_np_estado_dado = 1;
            TR("[np] estado del servicio -> en línea (callback)\n");
            llamar_guest(g_np_estado_cb, 3, g_np_estado_arg, 0);
        }
        for (int i = 0; i < 8; i++) if (g_auth[i].pendiente) {
            g_auth[i].pendiente = 0;
            TR("[np] ticket %d listo (callback)\n", i + 1);
            llamar_guest(g_auth[i].cb, (uint32_t)(i + 1), 256, g_auth[i].arg);
        }
    }
    r0 = 0;
}
void vita_hle_sceNpGetServiceState(void) {   /* (state*) */
    if (!net_en_linea()) { r0 = NET_ERROR; return; }
    if (r0) vita_write32(r0, 3);   /* SCE_NP_SERVICE_STATE_ONLINE */
    r0 = 0;
}
void vita_hle_sceNpManagerGetNpId(void) {
    if (!net_en_linea() || !r0) { r0 = NET_ERROR; return; }
    escribir_npid(r0);
    r0 = 0;
}
void vita_hle_sceNpManagerGetCachedParam(void) {   /* SceNpManagerCacheParam { size; onlineId; npId; ... } */
    if (!net_en_linea() || !r0) { r0 = NET_ERROR; return; }
    uint32_t sz = vita_read32(r0);
    if (sz < 60 || sz > 0x400) sz = 60;
    memset(vita_mem_ptr(r0 + 4, sz - 4), 0, sz - 4);
    strncpy((char *)vita_mem_ptr(r0 + 4, 16), jugador(), 16);
    escribir_npid(r0 + 24);
    r0 = 0;
}
void vita_hle_sceNpManagerGetChatRestrictionFlag(void) {
    if (!net_en_linea()) { r0 = NET_ERROR; return; }
    if (r0) vita_write32(r0, 0);
    r0 = 0;
}
void vita_hle_sceNpManagerGetContentRatingFlag(void) {
    if (!net_en_linea()) { r0 = NET_ERROR; return; }
    if (r0) vita_write32(r0, 0);
    if (r1) vita_write32(r1, 18);
    r0 = 0;
}
void vita_hle_sceNpRegisterServiceStateCallback(void) {   /* (cb, userdata) */
    if (!net_en_linea()) { r0 = NET_ERROR; return; }
    g_np_estado_cb = r0; g_np_estado_arg = r1; g_np_estado_dado = 0;
    r0 = 0;
}

void vita_hle_sceNpLookupInit(void) { r0 = NET_ERROR; }
void vita_hle_sceNpLookupTerm(void) { r0 = NET_ERROR; }
void vita_hle_sceNpLookupCreateTitleCtx(void) { r0 = NET_ERROR; }
void vita_hle_sceNpLookupCreateRequest(void) { r0 = NET_ERROR; }
void vita_hle_sceNpLookupDeleteRequest(void) { r0 = NET_ERROR; }
void vita_hle_sceNpLookupPollAsync(void) { r0 = NET_ERROR; }
void vita_hle_sceNpLookupNpIdAsync(void) { r0 = NET_ERROR; }
void vita_hle_sceNpLookupUserProfileAsync(void) { r0 = NET_ERROR; }

void vita_hle_sceNpAuthInit(void) { r0 = net_en_linea() ? 0 : NET_ERROR; }
void vita_hle_sceNpAuthTerm(void) { r0 = net_en_linea() ? 0 : NET_ERROR; }
void vita_hle_sceNpAuthCreateStartRequest(void) {   /* SceNpAuthRequestParameter: ... +28 ticketCb, +32 cbArg */
    if (!net_en_linea() || !r0) { r0 = NET_ERROR; return; }
    int i; for (i = 0; i < 8 && (g_auth[i].cb || g_auth[i].pendiente); i++) {}
    if (i == 8) i = 0;
    g_auth[i].cb = vita_read32(r0 + 28); g_auth[i].arg = vita_read32(r0 + 32); g_auth[i].pendiente = 1;
    TR("[np] pedido ticket %d (cb 0x%08X)\n", i + 1, g_auth[i].cb);
    r0 = (uint32_t)(i + 1);
}
void vita_hle_sceNpAuthDestroyRequest(void) {
    if (!net_en_linea()) { r0 = NET_ERROR; return; }
    if (r0 >= 1 && r0 <= 8) { g_auth[r0 - 1].cb = 0; g_auth[r0 - 1].pendiente = 0; }
    r0 = 0;
}
void vita_hle_sceNpAuthGetTicket(void) {   /* (id, buf, len): ticket falso con el nombre del jugador */
    if (!net_en_linea()) { r0 = NET_ERROR; return; }
    uint8_t t[256]; memset(t, 0, sizeof(t));
    snprintf((char *)t, sizeof(t), "PBTICKET:%s", jugador());
    uint32_t n = r2 < sizeof(t) ? r2 : (uint32_t)sizeof(t);
    if (r1 && n) memcpy(vita_mem_ptr(r1, n), t, n);
    r0 = sizeof(t);
}
void vita_hle_sceNpAuthGetEntitlementIdList(void) { r0 = net_en_linea() ? 0 : NET_ERROR; }

void vita_hle_sceNpBasicInit(void) { r0 = net_en_linea() ? 0 : NET_ERROR; }
void vita_hle_sceNpBasicTerm(void) { r0 = net_en_linea() ? 0 : NET_ERROR; }
void vita_hle_sceNpBasicCheckCallback(void) { r0 = net_en_linea() ? 0 : NET_ERROR; }
void vita_hle_sceNpBasicRegisterHandler(void) { r0 = net_en_linea() ? 0 : NET_ERROR; }
void vita_hle_sceNpBasicUnregisterHandler(void) { r0 = net_en_linea() ? 0 : NET_ERROR; }
void vita_hle_sceNpBasicSetInGamePresence(void) { r0 = net_en_linea() ? 0 : NET_ERROR; }
void vita_hle_sceNpBasicSendInGameDataMessage(void) { r0 = net_en_linea() ? 0 : NET_ERROR; }

void vita_hle_sceNpMessageInit(void) { r0 = NET_ERROR; }
void vita_hle_sceNpMessageTerm(void) { r0 = NET_ERROR; }
void vita_hle_sceNpMessageGetAttachedData(void) { r0 = NET_ERROR; }
void vita_hle_sceNpMessageSetAttachedDataUsedFlag(void) { r0 = NET_ERROR; }

void vita_hle_sceNpActivityInit(void) { r0 = NET_ERROR; }
void vita_hle_sceNpActivityPostStatus(void) { r0 = NET_ERROR; }

void vita_hle_sceNpCommerce2Init(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2Term(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2CreateCtx(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2DestroyCtx(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2CreateSessionStart(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2CreateSessionGetResult(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2CreateSessionCreateReq(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2DestroyReq(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2GetCategoryContentsStart(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2GetCategoryContentsGetResult(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2GetCategoryContentsCreateReq(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2InitGetCategoryContentsResult(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2GetCategoryInfo(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2GetCategoryInfoFromContentInfo(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2GetContentInfo(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2GetGameProductInfoFromContentInfo(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2GetGameSkuInfoFromGameProductInfo(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2GetPrice(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2GetSessionInfo(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2ShowPsStoreIcon(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2HidePsStoreIcon(void) { r0 = NET_ERROR; }

void vita_hle_sceNpTrophyInit(void) { r0 = NET_ERROR; }
void vita_hle_sceNpTrophyTerm(void) { r0 = NET_ERROR; }
void vita_hle_sceNpTrophyCreateHandle(void) { r0 = NET_ERROR; }
void vita_hle_sceNpTrophyCreateContext(void) { r0 = NET_ERROR; }
void vita_hle_sceNpTrophyDestroyHandle(void) { r0 = NET_ERROR; }
void vita_hle_sceNpTrophyUnlockTrophy(void) { r0 = NET_ERROR; }
