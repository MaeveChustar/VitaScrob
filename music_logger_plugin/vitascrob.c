#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/rtc.h>
#include <taihen.h>
#include <stdint.h>
#include <stdarg.h>

// Rockbox scrobbler log - readable by QtScrobbler and compatible tools
#define SCROBBLER_LOG  "ux0:data/vitascrob/.scrobbler.log"
#define SCROBBLER_DIR  "ux0:data/vitascrob"
#define MAX_LINE       1024
#define TARGET_MODULE  "SceMusicBrowser"

// Offsets (3.65 firmware)
#define OFF_FUN_810848A2 0x000848A2
#define OFF_FUN_81084B7A 0x00084B7A
#define OFF_FUN_81084BCC 0x00084BCC
#define OFF_FUN_8104CDEA 0x0004CDEA
#define OFF_FUN_81011B38 0x00011B38

static tai_hook_ref_t g_ref_848a2;
static tai_hook_ref_t g_ref_84b7a;
static tai_hook_ref_t g_ref_84bcc;
static tai_hook_ref_t g_ref_4cdea;
static tai_hook_ref_t g_ref_11b38;

static SceUID g_uid_848a2 = -1;
static SceUID g_uid_84b7a = -1;
static SceUID g_uid_84bcc = -1;
static SceUID g_uid_4cdea = -1;
static SceUID g_uid_11b38 = -1;

// Current track state
static char     g_title[256]        = {0};
static char     g_artist[256]       = {0};
static char     g_album[256]        = {0};
static uint32_t g_duration          = 0;   // milliseconds
static uint64_t g_track_start_time  = 0;   // unix seconds
static int      g_track_started     = 0;

// ----------------------------------------------------------------
// Utilities
// ----------------------------------------------------------------

static int is_valid_ptr(uintptr_t p)
{
    return (p >= 0x80000000 && p < 0xC0000000);
}

static uint64_t get_unix_timestamp(void)
{
    SceRtcTick tick;
    sceRtcGetCurrentTick(&tick);
    return tick.tick / 1000000ULL;
}

// ----------------------------------------------------------------
// Scrobbler log
// ----------------------------------------------------------------

static void ensure_log_header(void)
{
    // Create directory if needed
    sceIoMkdir(SCROBBLER_DIR, 0777);

    // Only write header if file doesn't exist yet
    SceUID fd = sceIoOpen(SCROBBLER_LOG, SCE_O_RDONLY, 0);
    if (fd >= 0) {
        sceIoClose(fd);
        return;  // already exists, header already written
    }

    fd = sceIoOpen(SCROBBLER_LOG, SCE_O_WRONLY | SCE_O_CREAT, 0777);
    if (fd >= 0) {
        const char *header =
            "#AUDIOSCROBBLER/1.1\n"
            "#TZ/UTC\n"
            "#CLIENT/VitaScrob 1.0\n";
        sceIoWrite(fd, header, sceClibStrnlen(header, 256));
        sceIoClose(fd);
    }
}

// Append one scrobble entry in Rockbox format:
// Artist|Album|Title|Track|Timestamp|Rating|Duration|MBTrackID
static void write_scrobble(const char *artist, const char *album,
                            const char *title, uint64_t timestamp,
                            uint32_t duration_ms)
{
    char buf[MAX_LINE];
    sceClibSnprintf(buf, sizeof(buf),
        "%s|%s|%s||%llu|L|%u|\n",
        artist,
        album,
        title,
        timestamp,
        duration_ms / 1000);  // convert ms to seconds for Rockbox format

    SceUID fd = sceIoOpen(SCROBBLER_LOG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, buf, sceClibStrnlen(buf, MAX_LINE));
        sceIoClose(fd);
    }
}

// Check previous track against Last.fm scrobble rules:
// - Duration > 30 seconds
// - Played > 50% of duration OR > 4 minutes
static void maybe_scrobble_previous(void)
{
    if (!g_track_started || g_title[0] == '\0') return;
    if (g_duration <= 30000) return;

    uint64_t now        = get_unix_timestamp();
    uint64_t elapsed_ms = (now - g_track_start_time) * 1000;
    uint32_t threshold  = g_duration / 2;
    if (threshold > 240000) threshold = 240000;  // cap at 4 minutes

    if (elapsed_ms >= threshold) {
        write_scrobble(g_artist, g_album, g_title,
                       g_track_start_time, g_duration);
    }
}

// ----------------------------------------------------------------
// String field reader
// ----------------------------------------------------------------

static void read_string_field(int owner, uint32_t offset, char *out, uint32_t outlen)
{
    out[0] = '\0';
    if (!is_valid_ptr((uintptr_t)owner + offset)) return;
    uintptr_t maybe_ptr = *(uint32_t *)(owner + offset);
    const char *src = is_valid_ptr(maybe_ptr)
        ? (const char *)maybe_ptr
        : (const char *)(owner + offset);
    sceClibStrncpy(out, src, outlen - 1);
    out[outlen - 1] = '\0';
}

// ----------------------------------------------------------------
// Hooks - only the metadata hook does real work now
// ----------------------------------------------------------------

static void hook_FUN_810848A2(int param_1)
{
    TAI_CONTINUE(void, g_ref_848a2, param_1);
}

static int hook_FUN_81084B7A(int param_1, char param_2)
{
    return TAI_CONTINUE(int, g_ref_84b7a, param_1, param_2);
}

static int hook_FUN_81084BCC(int param_1)
{
    return TAI_CONTINUE(int, g_ref_84bcc, param_1);
}

static int hook_FUN_8104CDEA(int param_1)
{
    return TAI_CONTINUE(int, g_ref_4cdea, param_1);
}

static void hook_FUN_81011B38(int param_1, char p2, char p3, int meta, char p5)
{
    if (is_valid_ptr((uintptr_t)param_1)) {
        char title[256], artist[256], album[256];
        uint32_t duration = 0;

        read_string_field(param_1, 0x248, title,  sizeof(title));
        read_string_field(param_1, 0x254, artist, sizeof(artist));
        read_string_field(param_1, 0x260, album,  sizeof(album));

        if (is_valid_ptr((uintptr_t)param_1 + 0x26C))
            duration = *(uint32_t *)(param_1 + 0x26C);

        // Only act on complete data for a new track
        if (title[0] != '\0' && artist[0] != '\0' && duration > 0) {
            if (sceClibStrncmp(title,  g_title,  255) != 0 ||
                sceClibStrncmp(artist, g_artist, 255) != 0) {

                maybe_scrobble_previous();

                sceClibStrncpy(g_title,  title,  255);
                sceClibStrncpy(g_artist, artist, 255);
                sceClibStrncpy(g_album,  album,  255);
                g_duration         = duration;
                g_track_start_time = get_unix_timestamp();
                g_track_started    = 1;
            }
        }
    }

    TAI_CONTINUE(void, g_ref_11b38, param_1, p2, p3, meta, p5);
}

// ----------------------------------------------------------------
// Setup / teardown
// ----------------------------------------------------------------

static void setup_hooks(void)
{
    tai_module_info_t info;
    sceClibMemset(&info, 0, sizeof(info));
    info.size = sizeof(info);

    if (taiGetModuleInfo(TARGET_MODULE, &info) < 0) return;

    g_uid_848a2 = taiHookFunctionOffset(&g_ref_848a2, info.modid, 0, OFF_FUN_810848A2, 1, hook_FUN_810848A2);
    g_uid_84b7a = taiHookFunctionOffset(&g_ref_84b7a, info.modid, 0, OFF_FUN_81084B7A, 1, hook_FUN_81084B7A);
    g_uid_84bcc = taiHookFunctionOffset(&g_ref_84bcc, info.modid, 0, OFF_FUN_81084BCC, 1, hook_FUN_81084BCC);
    g_uid_4cdea = taiHookFunctionOffset(&g_ref_4cdea, info.modid, 0, OFF_FUN_8104CDEA, 1, hook_FUN_8104CDEA);
    g_uid_11b38 = taiHookFunctionOffset(&g_ref_11b38, info.modid, 0, OFF_FUN_81011B38, 1, hook_FUN_81011B38);
}

static void release_hooks(void)
{
    if (g_uid_848a2 >= 0) taiHookRelease(g_uid_848a2, g_ref_848a2);
    if (g_uid_84b7a >= 0) taiHookRelease(g_uid_84b7a, g_ref_84b7a);
    if (g_uid_84bcc >= 0) taiHookRelease(g_uid_84bcc, g_ref_84bcc);
    if (g_uid_4cdea >= 0) taiHookRelease(g_uid_4cdea, g_ref_4cdea);
    if (g_uid_11b38 >= 0) taiHookRelease(g_uid_11b38, g_ref_11b38);
}

// ----------------------------------------------------------------
// Entry points
// ----------------------------------------------------------------

int _start(SceSize argc, const void *args) __attribute__((weak, alias("module_start")));

int module_start(SceSize argc, const void *args)
{
    ensure_log_header();
    setup_hooks();
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
    maybe_scrobble_previous();
    release_hooks();
    return SCE_KERNEL_STOP_SUCCESS;
}