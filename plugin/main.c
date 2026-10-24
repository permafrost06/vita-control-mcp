#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/sysmodule.h>
#include <taihen.h>
#include <string.h>
#include "core.h"

#define TOKEN_PATH "ux0:data/vita-control/token.txt"
#define LOG_PATH "ux0:data/vita-control/plugin.log"
#define HOOK_COUNT 13

static tai_hook_ref_t refs[HOOK_COUNT];
static SceUID hooks[HOOK_COUNT];
static SceUID input_mutex = -1;
static unsigned ctrl_hook_count;
static char token[65];
static size_t token_len;
static uint32_t input_buttons, input_flags;
static uint8_t input_axes[4];
static uint64_t input_until;
/* IDLE -> REQUESTED -> COPYING -> READY. The hook never touches READY data. */
enum { IDLE, REQUESTED, COPYING, READY };
static int capture_state;
static uint32_t frame_width, frame_height, frame_status;
static uint8_t frame[VC_MAX_RGB];
static uint8_t net_memory[256 * 1024] __attribute__((aligned(64)));

static void log_line(const char *line) {
    SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0600);
    if (fd >= 0) {
        sceIoWrite(fd, line, strlen(line));
        sceIoWrite(fd, "\n", 1);
        sceIoClose(fd);
    }
}

static int read_token(void) {
    SceUID fd = sceIoOpen(TOKEN_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) return -1;
    /* Reject truncation: the extra byte catches overlong files. */
    char buf[67];
    int n = sceIoRead(fd, buf, sizeof(buf));
    sceIoClose(fd);
    if (n < 0 || n == (int)sizeof(buf)) return -1;
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) --n;
    if (n < 32 || n > 64) return -1;
    for (int i = 0; i < n; ++i) {
        if (buf[i] < 33 || buf[i] > 126) return -1;
        token[i] = buf[i];
    }
    token_len = n;
    return 0;
}

static void release_input(void) {
    sceKernelLockMutex(input_mutex, 1, NULL);
    input_until = 0;
    input_buttons = input_flags = 0;
    sceKernelUnlockMutex(input_mutex, 1);
}

static void apply_input(int port, SceCtrlData *pads, int count, int negative, int ext) {
    if (port != 0 || !pads || count <= 0) return;
    sceKernelLockMutex(input_mutex, 1, NULL);
    uint64_t now = sceKernelGetProcessTimeWide();
    if (input_until > now) {
        uint32_t mask = vc_map_buttons(input_buttons, ext);
        for (int i = 0; i < count; ++i) {
            if (negative) pads[i].buttons &= ~mask;
            else pads[i].buttons |= mask;
            if (input_flags & 1) { pads[i].lx = input_axes[0]; pads[i].ly = input_axes[1]; }
            if (input_flags & 2) { pads[i].rx = input_axes[2]; pads[i].ry = input_axes[3]; }
        }
    }
    sceKernelUnlockMutex(input_mutex, 1);
}

#define CTRL_HOOK(name, index, neg, ext) \
    static int name(int port, SceCtrlData *pads, int count) { \
        int n = TAI_CONTINUE(int, refs[index], port, pads, count); \
        if (n > 0 && n <= count) apply_input(port, pads, n, neg, ext); \
        return n; \
    }
CTRL_HOOK(peek_positive, 0, 0, 0)
CTRL_HOOK(read_positive, 1, 0, 0)
CTRL_HOOK(peek_positive2, 2, 0, 1)
CTRL_HOOK(read_positive2, 3, 0, 1)
CTRL_HOOK(peek_ext, 4, 0, 0)
CTRL_HOOK(read_ext, 5, 0, 0)
CTRL_HOOK(peek_ext2, 6, 0, 1)
CTRL_HOOK(read_ext2, 7, 0, 1)
CTRL_HOOK(peek_negative, 8, 1, 0)
CTRL_HOOK(read_negative, 9, 1, 0)
CTRL_HOOK(peek_negative2, 10, 1, 1)
CTRL_HOOK(read_negative2, 11, 1, 1)

static int set_framebuffer(const SceDisplayFrameBuf *fb, SceDisplaySetBufSync sync) {
    /* Copy only on demand, on the submitting thread while the buffer is valid.
       No framebuffer pointer is retained across frames. */
    if (__atomic_load_n(&capture_state, __ATOMIC_ACQUIRE) == REQUESTED && fb &&
        fb->size >= sizeof(*fb) && fb->base &&
        __sync_bool_compare_and_swap(&capture_state, REQUESTED, COPYING)) {
        frame_width = fb->width;
        frame_height = fb->height;
        frame_status = vc_copy_rgb(frame, fb->base, fb->width, fb->height,
                                   fb->pitch, fb->pixelformat) == 0
                       ? VC_OK : VC_UNSUPPORTED_FRAME;
        __atomic_store_n(&capture_state, READY, __ATOMIC_RELEASE);
    }
    return TAI_CONTINUE(int, refs[12], fb, sync);
}

static int recv_all(int fd, void *buf, unsigned size) {
    uint8_t *p = buf;
    while (size) {
        int n = sceNetRecv(fd, p, size, 0);
        if (n <= 0) return -1;
        p += n; size -= n;
    }
    return 0;
}
static int send_all(int fd, const void *buf, unsigned size) {
    const uint8_t *p = buf;
    while (size) {
        int n = sceNetSend(fd, p, size, 0);
        if (n <= 0) return -1;
        p += n; size -= n;
    }
    return 0;
}
static int response_header(int fd, uint32_t op, uint32_t status, uint32_t size) {
    uint8_t hdr[16] = {'V', 'C', '0', '1'};
    vc_write32(hdr + 4, op | 0x80000000u);
    vc_write32(hdr + 8, size + 4);
    vc_write32(hdr + 12, status);
    return send_all(fd, hdr, sizeof(hdr));
}
static int screenshot(int fd) {
    __atomic_store_n(&capture_state, REQUESTED, __ATOMIC_RELEASE);
    uint64_t deadline = sceKernelGetProcessTimeWide() + 2000000;
    while (__atomic_load_n(&capture_state, __ATOMIC_ACQUIRE) != READY) {
        if (sceKernelGetProcessTimeWide() >= deadline &&
            __sync_bool_compare_and_swap(&capture_state, REQUESTED, IDLE))
            return response_header(fd, VC_SCREENSHOT, VC_NO_FRAME, 0);
        sceKernelDelayThread(1000);
    }
    int result;
    if (frame_status != VC_OK) {
        result = response_header(fd, VC_SCREENSHOT, frame_status, 0);
    } else {
        uint8_t dims[8];
        vc_write32(dims, frame_width); vc_write32(dims + 4, frame_height);
        unsigned size = frame_width * frame_height * 3;
        result = response_header(fd, VC_SCREENSHOT, VC_OK, size + 8);
        if (result == 0) result = send_all(fd, dims, sizeof(dims));
        if (result == 0) result = send_all(fd, frame, size);
    }
    __atomic_store_n(&capture_state, IDLE, __ATOMIC_RELEASE);
    return result;
}

static void serve_client(int fd) {
    int authenticated = 0;
    for (;;) {
        uint8_t hdr[12], payload[VC_MAX_REQUEST];
        if (recv_all(fd, hdr, sizeof(hdr)) < 0 || memcmp(hdr, "VC01", 4)) break;
        uint32_t op = vc_read32(hdr + 4), size = vc_read32(hdr + 8);
        if (size > sizeof(payload)) {
            response_header(fd, op, VC_BAD_REQUEST, 0);
            break;
        }
        if (recv_all(fd, payload, size) < 0) break;
        if (!authenticated) {
            if (op != VC_AUTH || !vc_token_equal(payload, size, token, token_len)) {
                response_header(fd, op, VC_UNAUTHORIZED, 0);
                break;
            }
            authenticated = 1;
            if (response_header(fd, op, VC_OK, 0) < 0) break;
            continue;
        }
        int result;
        if (op == VC_INFO && size == 0) {
            uint8_t info[16];
            vc_write32(info, 1); vc_write32(info + 4, ctrl_hook_count);
            vc_write32(info + 8, hooks[12] >= 0); vc_write32(info + 12, VC_MAX_DURATION);
            result = response_header(fd, op, VC_OK, sizeof(info));
            if (result == 0) result = send_all(fd, info, sizeof(info));
        } else if (op == VC_SCREENSHOT && size == 0) {
            result = screenshot(fd);
        } else if (op == VC_INPUT && vc_valid_input(payload, size)) {
            sceKernelLockMutex(input_mutex, 1, NULL);
            input_buttons = vc_read32(payload);
            input_flags = vc_read32(payload + 12);
            memcpy(input_axes, payload + 8, 4);
            input_until = sceKernelGetProcessTimeWide() + vc_read32(payload + 4) * 1000ull;
            sceKernelUnlockMutex(input_mutex, 1);
            result = response_header(fd, op, VC_OK, 0);
        } else if (op == VC_RELEASE && size == 0) {
            release_input();
            result = response_header(fd, op, VC_OK, 0);
        } else {
            result = response_header(fd, op, VC_BAD_REQUEST, 0);
        }
        if (result < 0) break;
    }
    release_input();
}

static int server_thread(SceSize argc, void *argv) {
    (void)argc; (void)argv;
    if (!sceSysmoduleIsLoaded(SCE_SYSMODULE_NET)) {
        /* Already loaded. */
    } else if (sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0) {
        log_line("Cannot load SceNet.");
        return 0;
    }
    /* Use a game's existing network stack when present. Do not terminate it. */
    SceNetInitParam init = {net_memory, sizeof(net_memory), 0};
    int fd = sceNetSocket("vita-control", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (fd == (int)SCE_NET_ERROR_ENOTINIT) {
        if (sceNetInit(&init) < 0) { log_line("Cannot initialize SceNet."); return 0; }
        fd = sceNetSocket("vita-control", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    }
    if (fd < 0) { log_line("Cannot create socket."); return 0; }
    int yes = 1;
    sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR, &yes, sizeof(yes));
    SceNetSockaddrIn addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_len = sizeof(addr);
    addr.sin_family = SCE_NET_AF_INET;
    addr.sin_port = sceNetHtons(VC_PORT);
    if (sceNetBind(fd, (SceNetSockaddr *)&addr, sizeof(addr)) < 0 || sceNetListen(fd, 1) < 0) {
        log_line("Cannot bind TCP port 5000; another game may own it.");
        sceNetSocketClose(fd);
        return 0;
    }
    log_line("Listening on TCP port 5000.");
    for (;;) {
        int client = sceNetAccept(fd, NULL, NULL);
        if (client < 0) { sceKernelDelayThread(100000); continue; }
        /* Vita SceNet socket timeouts are integer microseconds. */
        int timeout = 15000000;
        if (sceNetSetsockopt(client, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO,
                            &timeout, sizeof(timeout)) < 0 ||
            sceNetSetsockopt(client, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO,
                            &timeout, sizeof(timeout)) < 0) {
            sceNetSocketClose(client);
            continue;
        }
        serve_client(client);
        sceNetSocketClose(client);
    }
    return 0;
}

static void remove_hooks(void) {
    for (int i = HOOK_COUNT - 1; i >= 0; --i)
        if (hooks[i] >= 0) taiHookRelease(hooks[i], refs[i]);
}
int module_start(SceSize argc, const void *args) {
    (void)argc; (void)args;
    for (int i = 0; i < HOOK_COUNT; ++i) hooks[i] = -1;
    if (read_token() < 0) {
        log_line("Startup refused: token.txt must contain 32-64 printable ASCII characters.");
        return SCE_KERNEL_START_FAILED;
    }
    input_mutex = sceKernelCreateMutex("vita-control-input", 0, 0, NULL);
    if (input_mutex < 0) return SCE_KERNEL_START_FAILED;
    const uint32_t nids[12] = {
        0xA9C3CED6, 0x67E7AB83, 0x15F81E8C, 0xC4226A3E,
        0xA59454D3, 0xE2D99296, 0x860BF292, 0xA7178860,
        0x104ED1A7, 0x15F96FB0, 0x81A89660, 0x27A0C5FB
    };
    const void *funcs[12] = {
        peek_positive, read_positive, peek_positive2, read_positive2,
        peek_ext, read_ext, peek_ext2, read_ext2,
        peek_negative, read_negative, peek_negative2, read_negative2
    };
    for (int i = 0; i < 12; ++i) {
        hooks[i] = taiHookFunctionImport(&refs[i], TAI_MAIN_MODULE, TAI_ANY_LIBRARY,
                                         nids[i], funcs[i]);
        if (hooks[i] >= 0) ++ctrl_hook_count;
    }
    hooks[12] = taiHookFunctionImport(&refs[12], TAI_MAIN_MODULE, TAI_ANY_LIBRARY,
                                      0x7A410B64, set_framebuffer);
    if (!ctrl_hook_count || hooks[12] < 0) {
        log_line("Startup refused: game main module needs controller and display imports.");
        remove_hooks();
        sceKernelDeleteMutex(input_mutex);
        return SCE_KERNEL_START_FAILED;
    }
    SceUID thread = sceKernelCreateThread("vita-control-server", server_thread,
                                         0x10000100, 32 * 1024, 0, 0, NULL);
    if (thread < 0 || sceKernelStartThread(thread, 0, NULL) < 0) {
        if (thread >= 0) sceKernelDeleteThread(thread);
        remove_hooks();
        sceKernelDeleteMutex(input_mutex);
        return SCE_KERNEL_START_FAILED;
    }
    log_line("Hooks installed. Starting server.");
    return SCE_KERNEL_START_SUCCESS;
}
int module_stop(SceSize argc, const void *args) {
    (void)argc; (void)args;
    /* Hooks and the server may be executing on other cores. Quit the game to
       unload safely; the process teardown reclaims all resources together. */
    return SCE_KERNEL_STOP_FAIL;
}
