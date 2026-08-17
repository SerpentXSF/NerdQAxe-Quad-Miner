#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_config.h"
#include "macros.h"
#include "ArduinoJson.h"
#include "psram_allocator.h"

#include <pthread.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#define NVS_CONFIG_NAMESPACE "main"
#define NVS_JSON_KEY         "json_cfg"

namespace Config
{

static const char *TAG = "nvs_config";

// ---------------------------------------------------------------------------
// In-memory config state
// ---------------------------------------------------------------------------
//
// Runtime code, including tasks with PSRAM-backed stacks, only reads/writes
// this document. NVS access is centralized here so the actual flash write path
// can run from a normal FreeRTOS task.

static PSRAMAllocator s_allocator;
static JsonDocument   s_doc(&s_allocator);
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool s_dirty       = false;
static bool s_initialized = false;

// The flush task is intentionally created with xTaskCreate(), not
// xTaskCreatePSRAM(). It is the only async path that writes config to NVS.
#define FLUSH_BIT BIT0
static EventGroupHandle_t s_flush_eg = nullptr;

// Serialize s_doc and write to NVS. Caller must hold s_mutex.
static bool write_to_nvs()
{
    size_t len = measureJson(s_doc) + 1;
    char *buf = (char *) MALLOC(len);
    if (!buf) {
        ESP_LOGE(TAG, "flush: MALLOC failed (%u bytes)", (unsigned) len);
        return false;
    }
    serializeJson(s_doc, buf, len);

    bool ok = false;
    nvs_handle h;
    esp_err_t err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_JSON_KEY, buf, len - 1);
        if (err == ESP_OK) {
            err = nvs_commit(h);
            if (err == ESP_OK) {
                ok = true;
                ESP_LOGI(TAG, "Config flushed (%u bytes)", (unsigned)(len - 1));
            } else {
                ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGE(TAG, "nvs_set_blob failed: %s", esp_err_to_name(err));
        }
        nvs_close(h);
    } else {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
    }

    free(buf);
    return ok;
}

static void flush_task(void *)
{
    ESP_LOGI(TAG, "Flush task started");
    for (;;) {
        xEventGroupWaitBits(s_flush_eg, FLUSH_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
        // Small delay to batch rapid successive sets
        vTaskDelay(pdMS_TO_TICKS(100));
        // Clear any bits that arrived during the delay
        xEventGroupClearBits(s_flush_eg, FLUSH_BIT);

        PThreadGuard guard(s_mutex);

        if (!s_dirty) continue;

        if (write_to_nvs()) {
            s_dirty = false;
        }
    }
}

// ---------------------------------------------------------------------------
// Migration: old individual NVS keys -> flat JSON document
//
// Keep the schema flat and sparse: copy only legacy keys that actually exist.
// Missing keys stay missing so board/runtime defaults continue to work exactly
// like they did with direct NVS access.
// ---------------------------------------------------------------------------

static void migrate_from_legacy(nvs_handle h)
{
    ESP_LOGI(TAG, "Migrating legacy NVS keys to JSON config ...");

    auto copyStr = [&](const char *key) {
        size_t size = 0;
        if (nvs_get_str(h, key, NULL, &size) != ESP_OK || size == 0) {
            return;
        }
        char *value = (char *) MALLOC(size);
        if (!value) {
            ESP_LOGW(TAG, "migration: no memory for key %s", key);
            return;
        }
        if (nvs_get_str(h, key, value, &size) == ESP_OK) {
            s_doc[key] = value;
        }
        free(value);
    };

    auto copyU16 = [&](const char *key) {
        uint16_t value = 0;
        if (nvs_get_u16(h, key, &value) == ESP_OK) {
            s_doc[key] = value;
        }
    };

    auto copyU64 = [&](const char *key) {
        uint64_t value = 0;
        if (nvs_get_u64(h, key, &value) == ESP_OK) {
            s_doc[key] = value;
        }
    };

    const char *string_keys[] = {
        NVS_CONFIG_WIFI_SSID,
        NVS_CONFIG_WIFI_PASS,
        NVS_CONFIG_HOSTNAME,
        NVS_CONFIG_STRATUM_URL,
        NVS_CONFIG_STRATUM_USER,
        NVS_CONFIG_STRATUM_PASS,
        NVS_CONFIG_STRATUM_FALLBACK_URL,
        NVS_CONFIG_STRATUM_FALLBACK_USER,
        NVS_CONFIG_STRATUM_FALLBACK_PASS,
        NVS_CONFIG_INFLUX_URL,
        NVS_CONFIG_INFLUX_TOKEN,
        NVS_CONFIG_INFLUX_BUCKET,
        NVS_CONFIG_INFLUX_ORG,
        NVS_CONFIG_INFLUX_PREFIX,
        NVS_CONFIG_ALERT_DISCORD_URL,
        NVS_CONFIG_SWARM,
        NVS_CONFIG_OTP_SECRET,
        NVS_CONFIG_OTP_SESSION_KEY,
        NVS_CONFIG_SV2_AUTHORITY_PUBKEY,
        NVS_CONFIG_FB_SV2_AUTHORITY_PUBKEY,
    };

    const char *u16_keys[] = {
        NVS_CONFIG_STRATUM_PORT,
        NVS_CONFIG_STRATUM_ENONCE_SUB,
        NVS_CONFIG_STRATUM_TLS,
        NVS_CONFIG_STRATUM_KEEPALIVE,
        NVS_CONFIG_STRATUM_FALLBACK_PORT,
        NVS_CONFIG_STRATUM_FALLBACK_ENONCE_SUB,
        NVS_CONFIG_STRATUM_FALLBACK_TLS,
        NVS_CONFIG_ASIC_FREQ,
        NVS_CONFIG_ASIC_VOLTAGE,
        NVS_CONFIG_ASIC_JOB_INTERVAL,
        NVS_CONFIG_FLIP_SCREEN,
        NVS_CONFIG_INVERT_SCREEN,
        NVS_CONFIG_FAN_PWM_POLARITY,
        NVS_CONFIG_AUTO_FAN_SPEED,
        NVS_CONFIG_FAN_SPEED,
        NVS_CONFIG_SELF_TEST,
        NVS_CONFIG_AUTO_SCREEN_OFF,
        NVS_CONFIG_OVERHEAT_TEMP,
        NVS_CONFIG_INFLUX_ENABLE,
        NVS_CONFIG_INFLUX_PORT,
        NVS_CONFIG_PID_TARGET_TEMP,
        NVS_CONFIG_PID_P,
        NVS_CONFIG_PID_I,
        NVS_CONFIG_PID_D,
        NVS_CONFIG_FAN1_SPEED,
        NVS_CONFIG_FAN1_MODE,
        NVS_CONFIG_FAN1_PID_TEMP,
        NVS_CONFIG_FAN1_PID_P,
        NVS_CONFIG_FAN1_PID_I,
        NVS_CONFIG_FAN1_PID_D,
        NVS_CONFIG_FAN1_OVERHEAT,
        NVS_CONFIG_FAN_PID_USE_MAX,
        NVS_CONFIG_ALERT_DISCORD_WATCHDOG_ENABLE,
        NVS_CONFIG_ALERT_DISCORD_BLOCK_FOUND_ENABLE,
        NVS_CONFIG_ALERT_DISCORD_BEST_DIFF,
        NVS_CONFIG_ALERT_DISCORD_COINBASE_VERIFY,
        NVS_CONFIG_SHOW_BLOCK_FOUND_ENABLE,
        NVS_CONFIG_CAN_ENABLED,
        NVS_CONFIG_COINBASE_VERIFY_MODE,
        NVS_CONFIG_COINBASE_MAX_FEE,
        NVS_CONFIG_COINBASE_VERIFY_FORCE,
        NVS_CONFIG_FB_COINBASE_VERIFY_MODE,
        NVS_CONFIG_FB_COINBASE_MAX_FEE,
        NVS_CONFIG_FB_COINBASE_VERIFY_FORCE,
        NVS_CONFIG_OTP_ENABLED,
        NVS_CONFIG_OTP_USED_MASK,
        NVS_CONFIG_POOL_MODE_BALANCE,
        NVS_CONFIG_POOL_MODE,
        NVS_CONFIG_STRATUM_PROTOCOL,
        NVS_CONFIG_SV2_CHANNEL_TYPE,
        NVS_CONFIG_FB_STRATUM_PROTOCOL,
        NVS_CONFIG_FB_SV2_CHANNEL_TYPE,
    };

    const char *u64_keys[] = {
        NVS_CONFIG_STRATUM_DIFFICULTY,
        NVS_CONFIG_VR_FREQUENCY,
        NVS_CONFIG_BEST_DIFF,
        NVS_TOTAL_FOUND_BLOCKS,
        NVS_CONFIG_OTP_LAST_STEP,
        NVS_CONFIG_OTP_BOOT_ID,
    };

    for (const char *key : string_keys) copyStr(key);
    for (const char *key : u16_keys) copyU16(key);
    for (const char *key : u64_keys) copyU64(key);

    if (s_doc[NVS_CONFIG_OVERHEAT_TEMP].is<uint16_t>() &&
        s_doc[NVS_CONFIG_OVERHEAT_TEMP].as<uint16_t>() == 0) {
        s_doc[NVS_CONFIG_OVERHEAT_TEMP] = 70;
        ESP_LOGW(TAG, "Overheat temp 0 not allowed, set to 70");
    }
    if (s_doc[NVS_CONFIG_AUTO_FAN_SPEED].is<uint16_t>() &&
        s_doc[NVS_CONFIG_AUTO_FAN_SPEED].as<uint16_t>() == 1) {
        s_doc[NVS_CONFIG_AUTO_FAN_SPEED] = 0;
        s_doc[NVS_CONFIG_FAN_SPEED] = 100;
        ESP_LOGW(TAG, "AFC deprecated, set manual 100%%");
    }

    // Migrate VReg overheat temp.
    // Single-fan boards: always disabled (pidUseMax handles VReg temp via PID).
    // Dual-fan boards: inherit ASIC overheat temp if not yet set.
#if defined(NERDAXE) || defined(NERDAXEGAMMA)
    s_doc[NVS_CONFIG_FAN1_OVERHEAT] = 0;
#else
    if (s_doc[NVS_CONFIG_FAN1_OVERHEAT].isNull()) {
        uint16_t vreg_default = s_doc[NVS_CONFIG_OVERHEAT_TEMP].isNull()
                                    ? CONFIG_OVERHEAT_TEMP
                                    : s_doc[NVS_CONFIG_OVERHEAT_TEMP].as<uint16_t>();
        ESP_LOGI(TAG, "Migrating VReg overheat temp from ASIC value: %u°C", vreg_default);
        s_doc[NVS_CONFIG_FAN1_OVERHEAT] = vreg_default;
    }
#endif

    ESP_LOGI(TAG, "Migration done (%u bytes in doc)", s_doc.memoryUsage());
}

// ═══════════════════════════════════════════════════════════════════════════
// Flush
// ═══════════════════════════════════════════════════════════════════════════

// Synchronous flush is only used during boot, before any PSRAM-stack tasks are
// running. Once the flush task exists, Config::flush() only signals it.
static void flush_sync()
{
    PThreadGuard guard(s_mutex);

    if (!s_dirty) return;

    if (write_to_nvs()) {
        s_dirty = false;
    }
}

void flush()
{
    if (s_flush_eg) {
        xEventGroupSetBits(s_flush_eg, FLUSH_BIT);
    } else {
        flush_sync();
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Init
// ═══════════════════════════════════════════════════════════════════════════

void init()
{
    if (s_initialized) return;

    nvs_handle h;
    esp_err_t err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        s_initialized = true;
        return;
    }

    // Try to load existing flat JSON config blob
    size_t blob_size = 0;
    err = nvs_get_blob(h, NVS_JSON_KEY, NULL, &blob_size);

    if (err == ESP_OK && blob_size > 0) {
        char *buf = (char *) MALLOC(blob_size + 1);
        if (buf && nvs_get_blob(h, NVS_JSON_KEY, buf, &blob_size) == ESP_OK) {
            buf[blob_size] = '\0';
            DeserializationError de = deserializeJson(s_doc, buf);
            free(buf);
            if (de) {
                ESP_LOGW(TAG, "JSON parse failed (%s), re-migrating", de.c_str());
                s_doc.clear();
                migrate_from_legacy(h);
                s_dirty = true;
            } else {
                ESP_LOGI(TAG, "Loaded JSON config (%u bytes)", (unsigned) blob_size);
            }
        } else {
            free(buf);
            migrate_from_legacy(h);
            s_dirty = true;
        }
    } else {
        // No json_cfg blob -> migrate from individual NVS keys
        migrate_from_legacy(h);
        s_dirty = true;
    }

    nvs_close(h);

    // Sync flush during boot, before the non-PSRAM flush task is running.
    if (s_dirty) flush_sync();

    s_initialized = true;

    // Start async flush task
    s_flush_eg = xEventGroupCreate();
    xTaskCreate(flush_task, "cfg_flush", 4096, NULL, 1, NULL);
}

JsonDocument& getDoc()   { return s_doc; }
void cfgLock()           { pthread_mutex_lock(&s_mutex); }
void cfgUnlock()         { pthread_mutex_unlock(&s_mutex); }

// ═══════════════════════════════════════════════════════════════════════════
// Legacy accessor functions — now backed by in-memory JSON doc
//
// The wrappers in nvs_config.h still use the original flat NVS key names.
// The JSON blob is only the storage container; the config schema remains flat.
// ═══════════════════════════════════════════════════════════════════════════

char* nvs_config_get_string(const char *key, const char *default_value)
{
    PThreadGuard guard(s_mutex);
    if (s_doc[key].is<const char*>()) {
        const char *v = s_doc[key].as<const char*>();
        return v ? strdup(v) : (default_value ? strdup(default_value) : nullptr);
    }
    return default_value ? strdup(default_value) : nullptr;
}

void nvs_config_set_string(const char *key, const char *value)
{
    PThreadGuard guard(s_mutex);
    s_doc[key] = value;
    s_dirty = true;
}

uint16_t nvs_config_get_u16(const char *key, const uint16_t default_value)
{
    PThreadGuard guard(s_mutex);
    return s_doc[key].isNull() ? default_value : s_doc[key].as<uint16_t>();
}

void nvs_config_set_u16(const char *key, const uint16_t value)
{
    PThreadGuard guard(s_mutex);
    s_doc[key] = value;
    s_dirty = true;
}

uint64_t nvs_config_get_u64(const char *key, const uint64_t default_value)
{
    PThreadGuard guard(s_mutex);
    return s_doc[key].isNull() ? default_value : s_doc[key].as<uint64_t>();
}

void nvs_config_set_u64(const char *key, const uint64_t value)
{
    PThreadGuard guard(s_mutex);
    s_doc[key] = value;
    s_dirty = true;
}

bool nvs_config_has_u16(const char *key)
{
    PThreadGuard guard(s_mutex);
    return !s_doc[key].isNull();
}

bool cfgHasKey(const char *path)
{
    PThreadGuard guard(s_mutex);
    return !s_doc[path].isNull();
}

// ═══════════════════════════════════════════════════════════════════════════
// Generic typed accessors (Step 2)
// ═══════════════════════════════════════════════════════════════════════════

// Returns pointer into JSON doc — caller must hold s_mutex
static const char* cfgGetStr_locked(const char *path, const char *def)
{
    JsonVariant v = s_doc[path];
    if (v.is<const char*>()) {
        const char *s = v.as<const char*>();
        return s ? s : def;
    }
    return def;
}

char* cfgGetStrAlloc(const char *path, const char *def)
{
    PThreadGuard guard(s_mutex);
    const char *value = cfgGetStr_locked(path, def);
    return value ? strdup(value) : nullptr;
}

void cfgSetStr(const char *path, const char *value)
{
    PThreadGuard guard(s_mutex);
    s_doc[path] = value;
    s_dirty = true;
}

uint16_t cfgGetU16(const char *path, uint16_t def)
{
    PThreadGuard guard(s_mutex);
    JsonVariant v = s_doc[path];
    return v.isNull() ? def : v.as<uint16_t>();
}

void cfgSetU16(const char *path, uint16_t value)
{
    PThreadGuard guard(s_mutex);
    s_doc[path] = value;
    s_dirty = true;
}

bool cfgGetBool(const char *path, bool def)
{
    PThreadGuard guard(s_mutex);
    JsonVariant v = s_doc[path];
    if (v.isNull()) return def;
    if (v.is<bool>()) return v.as<bool>();
    return v.as<uint16_t>() != 0;
}

void cfgSetBool(const char *path, bool value)
{
    PThreadGuard guard(s_mutex);
    s_doc[path] = value;
    s_dirty = true;
}

uint64_t cfgGetU64(const char *path, uint64_t def)
{
    PThreadGuard guard(s_mutex);
    JsonVariant v = s_doc[path];
    return v.isNull() ? def : v.as<uint64_t>();
}

void cfgSetU64(const char *path, uint64_t value)
{
    PThreadGuard guard(s_mutex);
    s_doc[path] = value;
    s_dirty = true;
}

uint64_t getBestDiff()
{
    return cfgGetU64(NVS_CONFIG_BEST_DIFF, 0);
}

void setBestDiff(uint64_t value)
{
    cfgSetU64(NVS_CONFIG_BEST_DIFF, value);
    flush();
}

uint32_t getTotalFoundBlocks()
{
    return (uint32_t) cfgGetU64(NVS_TOTAL_FOUND_BLOCKS, 0);
}

void setTotalFoundBlocks(uint32_t value)
{
    cfgSetU64(NVS_TOTAL_FOUND_BLOCKS, (uint64_t) value);
    flush();
}

// ═══════════════════════════════════════════════════════════════════════════
// Indexed multi-pool config accessors
//
// The flat JSON config doc is serialized to a single NVS blob, so path length is
// not bound by NVS's 15-char key limit — "stratumuser0" etc. are fine as paths.
// Each getter reads "<legacyBase><idx>"; if that path is absent it falls back to
// the legacy 2-pool key so unmigrated devices keep working (pool 0 -> primary,
// pool 1 -> fallback). Pools >= 2 have no legacy source and take the default.
// ═══════════════════════════════════════════════════════════════════════════

// Build "<base><idx>" into buf and return it.
static const char *poolKey(char *buf, size_t n, const char *base, int idx)
{
    snprintf(buf, n, "%s%d", base, idx);
    return buf;
}

char *getPoolURL(int idx)
{
    char key[24];
    poolKey(key, sizeof(key), NVS_CONFIG_STRATUM_URL, idx);
    if (!cfgHasKey(key)) {
        if (idx == 0) return getStratumURL();
        if (idx == 1) return getStratumFallbackURL();
    }
    return cfgGetStrAlloc(key, "");
}

char *getPoolUser(int idx)
{
    char key[24];
    poolKey(key, sizeof(key), NVS_CONFIG_STRATUM_USER, idx);
    if (!cfgHasKey(key)) {
        if (idx == 0) return getStratumUser();
        if (idx == 1) return getStratumFallbackUser();
    }
    return cfgGetStrAlloc(key, "");
}

char *getPoolPass(int idx)
{
    char key[24];
    poolKey(key, sizeof(key), NVS_CONFIG_STRATUM_PASS, idx);
    if (!cfgHasKey(key)) {
        if (idx == 0) return getStratumPass();
        if (idx == 1) return getStratumFallbackPass();
    }
    return cfgGetStrAlloc(key, "");
}

uint16_t getPoolPort(int idx)
{
    char key[24];
    poolKey(key, sizeof(key), NVS_CONFIG_STRATUM_PORT, idx);
    if (!cfgHasKey(key)) {
        if (idx == 0) return getStratumPortNumber();
        if (idx == 1) return getStratumFallbackPortNumber();
    }
    return cfgGetU16(key, CONFIG_STRATUM_PORT);
}

bool getPoolEnonceSub(int idx)
{
    char key[24];
    poolKey(key, sizeof(key), NVS_CONFIG_STRATUM_ENONCE_SUB, idx);
    if (!cfgHasKey(key)) {
        if (idx == 0) return isStratumEnonceSubscribe();
        if (idx == 1) return isStratumFallbackEnonceSubscribe();
    }
    return cfgGetU16(key, 0) != 0;
}

bool getPoolTLS(int idx)
{
    char key[24];
    poolKey(key, sizeof(key), NVS_CONFIG_STRATUM_TLS, idx);
    if (!cfgHasKey(key)) {
        if (idx == 0) return isStratumTLS();
        if (idx == 1) return isStratumFallbackTLS();
    }
    return cfgGetU16(key, 0) != 0;
}

uint16_t getPoolProtocol(int idx)
{
    char key[24];
    poolKey(key, sizeof(key), NVS_CONFIG_STRATUM_PROTOCOL, idx);
    if (!cfgHasKey(key)) {
        if (idx == 0) return getStratumProtocol();
        if (idx == 1) return getFallbackStratumProtocol();
    }
    return cfgGetU16(key, 0);
}

char *getPoolSV2AuthorityPubkey(int idx)
{
    char key[24];
    poolKey(key, sizeof(key), NVS_CONFIG_SV2_AUTHORITY_PUBKEY, idx);
    if (!cfgHasKey(key)) {
        if (idx == 0) return getSV2AuthorityPubkey();
        if (idx == 1) return getFallbackSV2AuthorityPubkey();
    }
    return cfgGetStrAlloc(key, "");
}

uint16_t getPoolSV2ChannelType(int idx)
{
    char key[24];
    poolKey(key, sizeof(key), NVS_CONFIG_SV2_CHANNEL_TYPE, idx);
    if (!cfgHasKey(key)) {
        if (idx == 0) return getSV2ChannelType();
        if (idx == 1) return getFallbackSV2ChannelType();
    }
    return cfgGetU16(key, 0);
}

void setPoolURL(int idx, const char *value)
{
    char key[24];
    cfgSetStr(poolKey(key, sizeof(key), NVS_CONFIG_STRATUM_URL, idx), value);
}

void setPoolUser(int idx, const char *value)
{
    char key[24];
    cfgSetStr(poolKey(key, sizeof(key), NVS_CONFIG_STRATUM_USER, idx), value);
}

void setPoolPass(int idx, const char *value)
{
    char key[24];
    cfgSetStr(poolKey(key, sizeof(key), NVS_CONFIG_STRATUM_PASS, idx), value);
}

void setPoolPort(int idx, uint16_t value)
{
    char key[24];
    cfgSetU16(poolKey(key, sizeof(key), NVS_CONFIG_STRATUM_PORT, idx), value);
}

void setPoolEnonceSub(int idx, bool value)
{
    char key[24];
    cfgSetU16(poolKey(key, sizeof(key), NVS_CONFIG_STRATUM_ENONCE_SUB, idx), value ? 1 : 0);
}

void setPoolTLS(int idx, bool value)
{
    char key[24];
    cfgSetU16(poolKey(key, sizeof(key), NVS_CONFIG_STRATUM_TLS, idx), value ? 1 : 0);
}

void setPoolProtocol(int idx, uint16_t value)
{
    char key[24];
    cfgSetU16(poolKey(key, sizeof(key), NVS_CONFIG_STRATUM_PROTOCOL, idx), value);
}

void setPoolSV2AuthorityPubkey(int idx, const char *value)
{
    char key[24];
    cfgSetStr(poolKey(key, sizeof(key), NVS_CONFIG_SV2_AUTHORITY_PUBKEY, idx), value);
}

void setPoolSV2ChannelType(int idx, uint16_t value)
{
    char key[24];
    cfgSetU16(poolKey(key, sizeof(key), NVS_CONFIG_SV2_CHANNEL_TYPE, idx), value);
}

uint16_t getPoolWeight(int idx)
{
    char key[24];
    // Default 25 for every pool: with only ratios mattering, N configured pools
    // split evenly regardless of N (25/25 = 1/2 each, 25/25/25/25 = 1/4 each).
    return cfgGetU16(poolKey(key, sizeof(key), NVS_CONFIG_POOL_WEIGHT, idx), 25);
}

void setPoolWeight(int idx, uint16_t value)
{
    char key[24];
    cfgSetU16(poolKey(key, sizeof(key), NVS_CONFIG_POOL_WEIGHT, idx), value);
}

// ---- Per-pool coinbase verification (indexed, legacy fallback for pools 0/1) ----

uint16_t getCoinbaseVerifyMode(int pool)
{
    char key[24];
    poolKey(key, sizeof(key), NVS_CONFIG_COINBASE_VERIFY_MODE_N, pool);
    if (!cfgHasKey(key)) {
        if (pool == 0) return cfgGetU16(NVS_CONFIG_COINBASE_VERIFY_MODE, 0);
        if (pool == 1) return cfgGetU16(NVS_CONFIG_FB_COINBASE_VERIFY_MODE, 0);
    }
    return cfgGetU16(key, 0);
}

void setCoinbaseVerifyMode(int pool, uint16_t v)
{
    char key[24];
    cfgSetU16(poolKey(key, sizeof(key), NVS_CONFIG_COINBASE_VERIFY_MODE_N, pool), v);
}

uint16_t getCoinbaseMaxFee(int pool)
{
    char key[24];
    poolKey(key, sizeof(key), NVS_CONFIG_COINBASE_MAX_FEE_N, pool);
    if (!cfgHasKey(key)) {
        if (pool == 0) return cfgGetU16(NVS_CONFIG_COINBASE_MAX_FEE, 30);
        if (pool == 1) return cfgGetU16(NVS_CONFIG_FB_COINBASE_MAX_FEE, 30);
    }
    return cfgGetU16(key, 30);
}

void setCoinbaseMaxFee(int pool, uint16_t v)
{
    char key[24];
    cfgSetU16(poolKey(key, sizeof(key), NVS_CONFIG_COINBASE_MAX_FEE_N, pool), v);
}

bool getCoinbaseVerifyForce(int pool)
{
    char key[24];
    poolKey(key, sizeof(key), NVS_CONFIG_COINBASE_VERIFY_FORCE_N, pool);
    if (!cfgHasKey(key)) {
        if (pool == 0) return cfgGetU16(NVS_CONFIG_COINBASE_VERIFY_FORCE, 0) != 0;
        if (pool == 1) return cfgGetU16(NVS_CONFIG_FB_COINBASE_VERIFY_FORCE, 0) != 0;
    }
    return cfgGetU16(key, 0) != 0;
}

void setCoinbaseVerifyForce(int pool, bool v)
{
    char key[24];
    cfgSetU16(poolKey(key, sizeof(key), NVS_CONFIG_COINBASE_VERIFY_FORCE_N, pool), v ? 1 : 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// Legacy migrate_config() — now just calls init()
// ═══════════════════════════════════════════════════════════════════════════

void migrate_config()
{
    init();
}

} // namespace Config
