#include "driver/tkl_jieli_audio_server_native.h"

#include <string.h>

#include "system/os/os_api.h"
#include "system/generic/printf.h"
#include "system/server/server_core.h"
#include "server/audio_server.h"

#define JIELI_AUDIO_PLAY_QUEUE_SIZE (16u * 1024u)
#define JIELI_AUDIO_CAPTURE_BUFFER_SIZE (8u * 1024u)
#define JIELI_AUDIO_CAPTURE_SESSION_COUNT 128u

/* The vendor audio tests release their VFS context after AUDIO_ENC_CLOSE.
 * Keep callback records in static storage and only recycle one after CLOSE
 * succeeds and all observed VFS callbacks have returned. */
typedef struct {
    JIELI_AUDIO_NATIVE_CAPTURE_CB callback;
    void *cookie;
    uint32_t callbacks_inflight;
    uint8_t allocated;
    uint8_t enabled;
    uint8_t stopped;
    uint8_t closed;
} AUDIO_CAPTURE_SESSION_T;

typedef struct {
    struct server *server;
    JIELI_AUDIO_NATIVE_CAPTURE_CB callback;
    void *cookie;
    AUDIO_CAPTURE_SESSION_T *current;
    uint32_t sample_rate;
    int volume;
    uint8_t initialized;
} AUDIO_CAPTURE_STATE_T;

typedef struct {
    struct server *server;
    OS_SEM data_sem;
    uint8_t queue[JIELI_AUDIO_PLAY_QUEUE_SIZE];
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t used;
    uint32_t sample_rate;
    int volume;
    uint8_t sem_ready;
    uint8_t initialized;
    uint8_t running;
} AUDIO_PLAY_STATE_T;

static OS_MUTEX s_audio_lock;
static uint8_t s_audio_lock_ready;
static AUDIO_CAPTURE_SESSION_T s_capture_sessions[JIELI_AUDIO_CAPTURE_SESSION_COUNT];
static AUDIO_CAPTURE_STATE_T s_capture;
static AUDIO_PLAY_STATE_T s_play;

static void __lock(void)
{
    (void)os_mutex_pend(&s_audio_lock, 0);
}

static void __unlock(void)
{
    (void)os_mutex_post(&s_audio_lock);
}

uintptr_t jieli_audio_native_critical_enter(void)
{
    __lock();
    return 1;
}

void jieli_audio_native_critical_exit(uintptr_t state)
{
    (void)state;
    __unlock();
}

int jieli_audio_native_prepare(void)
{
    int ret;
    if (s_audio_lock_ready) return 0;
    ret = os_mutex_create(&s_audio_lock);
    if (ret != 0) {
        printf("[JIELI_AUDIO] mutex creation failed: %d\n", ret);
        return -1;
    }
    s_audio_lock_ready = 1;
    return 0;
}

static void *__audio_vfs_open(const char *path, const char *mode)
{
    (void)path;
    (void)mode;
    return NULL;
}

static int __audio_vfs_fseek(void *file, u32 offset, int mode)
{
    (void)file;
    (void)offset;
    (void)mode;
    return -1;
}

static int __audio_vfs_ftell(void *file)
{
    (void)file;
    return -1;
}

static int __audio_vfs_flen(void *file)
{
    (void)file;
    return -1;
}

static int __audio_vfs_fclose(void *file)
{
    (void)file;
    return 0;
}

static int __capture_fwrite(void *file, void *data, u32 len)
{
    AUDIO_CAPTURE_SESSION_T *session = (AUDIO_CAPTURE_SESSION_T *)file;
    JIELI_AUDIO_NATIVE_CAPTURE_CB callback;
    void *cookie;

    if (session == NULL || data == NULL || len == 0) return 0;
    __lock();
    if (!session->enabled || session->callback == NULL) {
        __unlock();
        return (int)len;
    }
    ++session->callbacks_inflight;
    callback = session->callback;
    cookie = session->cookie;
    __unlock();

    callback(cookie, (const uint8_t *)data, len, 0);

    __lock();
    --session->callbacks_inflight;
    __unlock();
    return (int)len;
}

static const struct audio_vfs_ops s_capture_vfs = {
    .fopen = __audio_vfs_open,
    .fwrite = __capture_fwrite,
    .fseek = __audio_vfs_fseek,
    .ftell = __audio_vfs_ftell,
    .flen = __audio_vfs_flen,
    .fclose = __audio_vfs_fclose,
};

static void __audio_event(void *priv, int argc, int *argv)
{
    (void)priv;
    (void)argc;
    (void)argv;
}

static int __capture_open_session(void)
{
    union audio_req req;
    AUDIO_CAPTURE_SESSION_T *session = NULL;
    u32 i;
    int ret;

    __lock();
    for (i = 0; i < JIELI_AUDIO_CAPTURE_SESSION_COUNT; ++i) {
        if (!s_capture_sessions[i].allocated) {
            session = &s_capture_sessions[i];
            memset(session, 0, sizeof(*session));
            session->allocated = 1;
            session->enabled = 1;
            session->callback = s_capture.callback;
            session->cookie = s_capture.cookie;
            s_capture.current = session;
            break;
        }
    }
    __unlock();
    if (session == NULL) return -4;

    memset(&req, 0, sizeof(req));
    req.enc.cmd = AUDIO_ENC_OPEN;
    req.enc.channel = 1;
    req.enc.volume = (u8)s_capture.volume;
    req.enc.output_buf_len = JIELI_AUDIO_CAPTURE_BUFFER_SIZE;
    req.enc.sample_rate = s_capture.sample_rate;
    req.enc.format = "pcm";
    req.enc.sample_source = "mic";
    req.enc.frame_size = (u16)(s_capture.sample_rate / 25u); /* PCM bytes per vendor sample. */
    req.enc.no_auto_start = 1;
    req.enc.vfs_ops = &s_capture_vfs;
    req.enc.file = (FILE *)session;
    ret = server_request(s_capture.server, AUDIO_REQ_ENC, &req);
    if (ret != 0) {
        printf("[JIELI_AUDIO] encoder OPEN request failed: %d\n", ret);
        __lock();
        session->enabled = 0;
        session->callback = NULL;
        session->cookie = NULL;
        session->allocated = 0;
        s_capture.current = NULL;
        __unlock();
        return -1;
    }
    return 0;
}

int jieli_audio_native_ai_init(const JIELI_AUDIO_NATIVE_PCM_CONFIG_T *config,
                               JIELI_AUDIO_NATIVE_CAPTURE_CB callback, void *cookie)
{
    if (config == NULL || callback == NULL) {
        printf("[JIELI_AUDIO] ai_init rejected: config=%p callback_present=%u\n",
               (void *)config, callback != NULL ? 1u : 0u);
        return -2;
    }
    printf("[JIELI_AUDIO] ai_init config: rate=%u bits=%u channels=%u cookie=%p\n",
           (unsigned)config->sample_rate, (unsigned)config->bits_per_sample,
           (unsigned)config->channels, cookie);
    if (config->sample_rate != 16000 || config->bits_per_sample != 16 || config->channels != 1) {
        printf("[JIELI_AUDIO] ai_init unsupported PCM format\n");
        return -3;
    }
    if (s_capture.initialized) {
        printf("[JIELI_AUDIO] ai_init rejected: capture already initialized\n");
        return -4;
    }

    memset(&s_capture, 0, sizeof(s_capture));
    printf("[JIELI_AUDIO] opening audio_server mode=enc\n");
    s_capture.server = server_open("audio_server", "enc");
    printf("[JIELI_AUDIO] server_open encoder handle=%p\n", (void *)s_capture.server);
    if (s_capture.server == NULL) {
        printf("[JIELI_AUDIO] server_open failed: name=audio_server mode=enc (service missing or open rejected)\n");
        return -1;
    }
    printf("[JIELI_AUDIO] server_open encoder succeeded\n");
    server_register_event_handler(s_capture.server, NULL, __audio_event);
    s_capture.callback = callback;
    s_capture.cookie = cookie;
    s_capture.sample_rate = config->sample_rate;
    s_capture.volume = 50;
    s_capture.initialized = 1;
    return 0;
}

int jieli_audio_native_ai_start(void)
{
    union audio_req req;
    int ret;
    if (!s_capture.initialized || s_capture.server == NULL) {
        printf("[JIELI_AUDIO] ai_start rejected: initialized=%u server=%p\n",
               (unsigned)s_capture.initialized, (void *)s_capture.server);
        return -4;
    }
    ret = __capture_open_session();
    if (ret != 0) {
        printf("[JIELI_AUDIO] capture session open failed: %d\n", ret);
        return -1;
    }
    memset(&req, 0, sizeof(req));
    req.enc.cmd = AUDIO_ENC_START;
    ret = server_request(s_capture.server, AUDIO_REQ_ENC, &req);
    if (ret != 0) {
        printf("[JIELI_AUDIO] encoder START request failed: %d\n", ret);
        (void)jieli_audio_native_ai_stop();
        return -1;
    }
    return 0;
}

int jieli_audio_native_ai_stop(void)
{
    union audio_req req;
    AUDIO_CAPTURE_SESSION_T *session = s_capture.current;
    uint32_t inflight;
    uint32_t waited = 0;
    int stop_result = 0;

    if (session == NULL) return 0;
    __lock();
    session->enabled = 0;
    __unlock();

    if (!session->stopped) {
        memset(&req, 0, sizeof(req));
        req.enc.cmd = AUDIO_ENC_STOP;
        stop_result = server_request(s_capture.server, AUDIO_REQ_ENC, &req);
        if (stop_result != 0) return -1;
        __lock();
        session->stopped = 1;
        __unlock();
    }
    if (!session->closed) {
        memset(&req, 0, sizeof(req));
        req.enc.cmd = AUDIO_ENC_CLOSE;
        if (server_request(s_capture.server, AUDIO_REQ_ENC, &req) != 0) return -1;
        __lock();
        session->closed = 1;
        __unlock();
    }

    __lock();
    inflight = session->callbacks_inflight;
    __unlock();
    /* The session remains in static storage; wait for callbacks already in
     * progress before reporting a clean stop to TKL. */
    while (inflight != 0 && waited++ < 100u) {
        os_time_dly(1);
        __lock();
        inflight = session->callbacks_inflight;
        __unlock();
    }
    if (inflight != 0) {
        printf("[JIELI_AUDIO] encoder callbacks did not drain: %u\n", (unsigned)inflight);
        return -1;
    }

    __lock();
    session->callback = NULL;
    session->cookie = NULL;
    session->allocated = 0;
    session->stopped = 0;
    session->closed = 0;
    if (s_capture.current == session) s_capture.current = NULL;
    __unlock();
    return 0;
}

int jieli_audio_native_ai_uninit(void)
{
    if (!s_capture.initialized) return 0;
    if (s_capture.current != NULL && jieli_audio_native_ai_stop() != 0) return -1;
    server_close(s_capture.server);
    s_capture.server = NULL;
    s_capture.initialized = 0;
    s_capture.callback = NULL;
    s_capture.cookie = NULL;
    return 0;
}

int jieli_audio_native_ai_set_volume(int volume)
{
    union audio_req req;
    if (volume < 0 || volume > 100) return -2;
    if (!s_capture.initialized) return -4;
    s_capture.volume = volume;
    if (s_capture.current == NULL) return 0;
    memset(&req, 0, sizeof(req));
    req.enc.cmd = AUDIO_ENC_SET_VOLUME;
    req.enc.volume = (u8)volume;
    return server_request(s_capture.server, AUDIO_REQ_ENC, &req) == 0 ? 0 : -1;
}

int jieli_audio_native_ai_get_volume(int *volume)
{
    if (volume == NULL) return -2;
    if (!s_capture.initialized) return -4;
    *volume = s_capture.volume;
    return 0;
}

static int __play_fread(void *file, void *buffer, u32 length)
{
    AUDIO_PLAY_STATE_T *state = (AUDIO_PLAY_STATE_T *)file;
    if (state == NULL || buffer == NULL) return 0;
    if (length == 0) return 0;
    for (;;) {
        u32 chunk;
        u32 first;
        __lock();
        if (state->used == 0) {
            int running = state->running;
            __unlock();
            if (!running) break;
            (void)os_sem_pend(&state->data_sem, 0);
            continue;
        }
        chunk = length;
        if (chunk > state->used) chunk = state->used;
        first = JIELI_AUDIO_PLAY_QUEUE_SIZE - state->read_pos;
        if (first > chunk) first = chunk;
        memcpy((u8 *)buffer, state->queue + state->read_pos, first);
        if (chunk > first) memcpy((u8 *)buffer + first, state->queue, chunk - first);
        state->read_pos = (state->read_pos + chunk) % JIELI_AUDIO_PLAY_QUEUE_SIZE;
        state->used -= chunk;
        __unlock();
        /* Like the vendor PCM VFS, return once any bytes are available. A
         * final short chunk must not block waiting for a future write. */
        return (int)chunk;
    }
    return 0;
}

static const struct audio_vfs_ops s_play_vfs = {
    .fopen = __audio_vfs_open,
    .fread = __play_fread,
    .fseek = __audio_vfs_fseek,
    .ftell = __audio_vfs_ftell,
    .flen = __audio_vfs_flen,
    .fclose = __audio_vfs_fclose,
};

int jieli_audio_native_ao_init(const JIELI_AUDIO_NATIVE_PCM_CONFIG_T *config, void **stream)
{
    union audio_req req;
    int ret;
    if (config == NULL || stream == NULL) {
        printf("[JIELI_AUDIO] ao_init rejected: config=%p stream_out=%p\n",
               (void *)config, (void *)stream);
        return -2;
    }
    printf("[JIELI_AUDIO] ao_init config: rate=%u bits=%u channels=%u\n",
           (unsigned)config->sample_rate, (unsigned)config->bits_per_sample, (unsigned)config->channels);
    if (config->sample_rate != 16000 || config->bits_per_sample != 16 || config->channels != 1) {
        printf("[JIELI_AUDIO] ao_init unsupported PCM format\n");
        return -3;
    }
    if (s_play.initialized) {
        printf("[JIELI_AUDIO] ao_init rejected: playback already initialized\n");
        return -4;
    }
    if (!s_play.sem_ready) {
        ret = os_sem_create(&s_play.data_sem, 0);
        if (ret != 0) {
            printf("[JIELI_AUDIO] playback semaphore creation failed: %d\n", ret);
            return -1;
        }
        s_play.sem_ready = 1;
    }
    printf("[JIELI_AUDIO] opening audio_server mode=dec\n");
    s_play.server = server_open("audio_server", "dec");
    printf("[JIELI_AUDIO] server_open decoder handle=%p\n", (void *)s_play.server);
    if (s_play.server == NULL) {
        printf("[JIELI_AUDIO] server_open failed: name=audio_server mode=dec (service missing or open rejected)\n");
        return -1;
    }
    printf("[JIELI_AUDIO] server_open decoder succeeded\n");
    server_register_event_handler(s_play.server, NULL, __audio_event);
    s_play.sample_rate = config->sample_rate;
    s_play.volume = 50;
    s_play.read_pos = s_play.write_pos = s_play.used = 0;
    memset(&req, 0, sizeof(req));
    req.dec.cmd = AUDIO_DEC_OPEN;
    req.dec.channel = 1;
    req.dec.volume = (u8)s_play.volume;
    req.dec.output_buf_len = JIELI_AUDIO_PLAY_QUEUE_SIZE;
    req.dec.sample_rate = config->sample_rate;
    req.dec.dec_type = "pcm";
    req.dec.sample_source = "dac";
    req.dec.vfs_ops = &s_play_vfs;
    req.dec.file = (FILE *)&s_play;
    ret = server_request(s_play.server, AUDIO_REQ_DEC, &req);
    printf("[JIELI_AUDIO] decoder OPEN result=%d\n", ret);
    if (ret != 0) {
        printf("[JIELI_AUDIO] decoder OPEN request failed: %d\n", ret);
        server_close(s_play.server);
        s_play.server = NULL;
        return -1;
    }
    s_play.initialized = 1;
    *stream = &s_play;
    return 0;
}

int jieli_audio_native_ao_start(void *stream)
{
    union audio_req req;
    int ret;
    if (stream != &s_play || !s_play.initialized) return -2;
    __lock();
    s_play.running = 1;
    __unlock();
    memset(&req, 0, sizeof(req));
    req.dec.cmd = AUDIO_DEC_START;
    ret = server_request(s_play.server, AUDIO_REQ_DEC, &req);
    if (ret != 0) {
        printf("[JIELI_AUDIO] decoder START request failed: %d\n", ret);
        __lock();
        s_play.running = 0;
        __unlock();
        (void)os_sem_post(&s_play.data_sem);
        return -1;
    }
    return 0;
}

int jieli_audio_native_ao_stop(void *stream)
{
    union audio_req req;
    if (stream != &s_play || !s_play.initialized) return -2;
    __lock();
    s_play.running = 0;
    s_play.read_pos = s_play.write_pos = s_play.used = 0;
    __unlock();
    (void)os_sem_post(&s_play.data_sem);
    memset(&req, 0, sizeof(req));
    req.dec.cmd = AUDIO_DEC_STOP;
    return server_request(s_play.server, AUDIO_REQ_DEC, &req) == 0 ? 0 : -1;
}

int jieli_audio_native_ao_uninit(void *stream)
{
    if (stream != &s_play) return -2;
    if (!s_play.initialized) return 0;
    if (s_play.running && jieli_audio_native_ao_stop(stream) != 0) return -1;
    /* The WL82 PCM sample and both audio_server.h headers expose STOP; neither
     * SDK declares AUDIO_DEC_CLOSE. Match the vendor sample's STOP + close. */
    server_close(s_play.server);
    s_play.server = NULL;
    __lock();
    s_play.initialized = 0;
    s_play.running = 0;
    s_play.read_pos = s_play.write_pos = s_play.used = 0;
    __unlock();
    return 0;
}

int jieli_audio_native_ao_set_volume(void *stream, int volume)
{
    union audio_req req;
    if (stream != &s_play || volume < 0 || volume > 100) return -2;
    if (!s_play.initialized) return -4;
    memset(&req, 0, sizeof(req));
    req.dec.cmd = AUDIO_DEC_SET_VOLUME;
    req.dec.volume = (u8)volume;
    if (server_request(s_play.server, AUDIO_REQ_DEC, &req) != 0) return -1;
    s_play.volume = volume;
    return 0;
}

int jieli_audio_native_ao_get_volume(void *stream, int *volume)
{
    if (stream != &s_play || volume == NULL) return -2;
    if (!s_play.initialized) return -4;
    *volume = s_play.volume;
    return 0;
}

int jieli_audio_native_ao_write(void *stream, const uint8_t *data, size_t size)
{
    u32 first;
    if (stream != &s_play || data == NULL || size == 0 || size > JIELI_AUDIO_PLAY_QUEUE_SIZE || (size & 1u))
        return -2;
    __lock();
    if (!s_play.running) {
        __unlock();
        return -4;
    }
    if (size > JIELI_AUDIO_PLAY_QUEUE_SIZE - s_play.used) {
        __unlock();
        return -31;
    }
    first = JIELI_AUDIO_PLAY_QUEUE_SIZE - s_play.write_pos;
    if (first > size) first = (u32)size;
    memcpy(s_play.queue + s_play.write_pos, data, first);
    if (size > first) memcpy(s_play.queue, data + first, size - first);
    s_play.write_pos = (s_play.write_pos + (u32)size) % JIELI_AUDIO_PLAY_QUEUE_SIZE;
    s_play.used += (u32)size;
    __unlock();
    (void)os_sem_post(&s_play.data_sem);
    return 0;
}

int jieli_audio_native_ao_flush(void *stream)
{
    if (stream != &s_play) return -2;
    __lock();
    s_play.read_pos = s_play.write_pos = s_play.used = 0;
    __unlock();
    return 0;
}
