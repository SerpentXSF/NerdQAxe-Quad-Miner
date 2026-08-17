#include "handler_v2_settings.h"

#include <math.h>
#include <string.h>
#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "ArduinoJson.h"
#include "psram_allocator.h"
#include "global_state.h"
#include "nvs_config.h"
#include "http_cors.h"
#include "http_utils.h"
#include "macros.h"
#include "network_manager.h"

static const char *TAG = "http_v2_settings";

esp_err_t GET_V2_settings(httpd_req_t *req)
{
    ConGuard g(http_server, req);

    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    httpd_resp_set_type(req, "application/json");
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    Board* board = SYSTEM_MODULE.getBoard();

    PSRAMAllocator allocator;
    JsonDocument doc(&allocator);

    // --- device identity ---
    doc["asicModel"]   = board->getAsicModel();
    doc["deviceModel"] = board->getDeviceModel();
    doc["version"]     = esp_app_get_description()->version;
    doc["otp"]         = Config::isOTPEnabled();
    doc["apActive"]    = NETWORK.isApActive();

    // --- can ---
    {
        JsonObject can = doc["can"].to<JsonObject>();
        can["hasExtension"] = board->hasCanExtension();
        can["enabled"]      = Config::isCanEnabled();
    }

    // --- asic settings (current + defaults + options merged from /asic endpoint) ---
    doc["frequency"]        = board->getAsicFrequency();
    doc["coreVoltage"]      = board->getAsicVoltageMillis();
    doc["vrFrequency"]      = board->getVrFrequency();
    doc["defaultFrequency"] = board->getDefaultAsicFrequency();
    doc["defaultCoreVoltage"] = board->getDefaultAsicVoltageMillis();
    doc["defaultVrFrequency"] = board->getDefaultVrFrequency();
    doc["ecoFrequency"]     = board->getEcoAsicFrequency();
    doc["ecoCoreVoltage"]   = board->getEcoAsicVoltageMillis();
    doc["absMinCoreVoltage"] = board->getAbsMinAsicVoltageMillis();
    doc["absMaxCoreVoltage"] = board->getAbsMaxAsicVoltageMillis();
    {
        JsonArray arr = doc["frequencyOptions"].to<JsonArray>();
        for (uint32_t f : board->getFrequencyOptions()) arr.add(f);
    }
    {
        JsonArray arr = doc["voltageOptions"].to<JsonArray>();
        for (uint32_t v : board->getVoltageOptions()) arr.add(v);
    }

    // --- stratum / pools ---
    doc["poolMode"]        = Config::getPoolMode();
    doc["poolBalance"]     = Config::getPoolBalance();
    doc["stratumKeep"]    = Config::isStratumKeepaliveEnabled() ? 1 : 0;
    doc["jobInterval"]     = board->getAsicJobIntervalMs();
    doc["stratumDifficulty"] = Config::getStratumDifficulty();
    doc["maxPools"] = MAX_POOLS;
    {
        JsonArray pools = doc["pools"].to<JsonArray>();

        // Number of configured pools = contiguous non-empty URLs, floored at 2
        // (primary + fallback always shown). Mirrors StratumManager's boot count.
        int numPools = 0;
        for (int i = 0; i < MAX_POOLS; i++) {
            char *u = Config::getPoolURL(i);
            bool has = u && strlen(u) > 0;
            safe_free(u);
            if (!has) break;
            numPools++;
        }
        if (numPools < 2) numPools = 2;

        for (int i = 0; i < numPools; i++) {
            JsonObject pool = pools.add<JsonObject>();
            char *url  = Config::getPoolURL(i);
            char *user = Config::getPoolUser(i);
            char *sv2  = Config::getPoolSV2AuthorityPubkey(i);
            pool["url"]              = url;
            pool["port"]             = Config::getPoolPort(i);
            pool["user"]             = user;
            pool["enonceSubscribe"]  = Config::getPoolEnonceSub(i);
            pool["tls"]              = Config::getPoolTLS(i);
            pool["protocol"]         = Config::getPoolProtocol(i);
            pool["sv2AuthorityPubkey"] = sv2;
            pool["sv2ChannelType"]   = Config::getPoolSV2ChannelType(i);
            // Coinbase verification is only stored for pools 0/1.
            pool["coinbaseVerifyMode"]  = (i < 2) ? Config::getCoinbaseVerifyMode(i) : 0;
            pool["coinbaseMaxFee"]      = ((i < 2) ? Config::getCoinbaseMaxFee(i) : 30) / 10.0f;
            pool["coinbaseVerifyForce"] = (i < 2) ? Config::getCoinbaseVerifyForce(i) : false;
            pool["weight"]           = Config::getPoolWeight(i);
            safe_free(url);
            safe_free(user);
            safe_free(sv2);
        }
    }

    // --- fans ---
    {
        JsonArray fans = doc["fans"].to<JsonArray>();
        int numFans = board->getNumFans();
        for (int ch = 0; ch < numFans; ch++) {
            PidSettings* fanPid = board->getPidSettings(ch);
            JsonObject fan = fans.add<JsonObject>();
            fan["label"]       = board->getFanLabel(ch);
            fan["mode"]        = Config::getFanMode(ch);
            fan["manualSpeed"] = Config::getFanManualSpeed(ch);
            fan["overheatTemp"] = Config::getFanOverheatTemp(ch);
            JsonObject pid_obj = fan["pid"].to<JsonObject>();
            pid_obj["targetTemp"] = board->isPIDAvailable() ? (int) fanPid->targetTemp : -1;
            pid_obj["p"]          = (float) fanPid->p / 100.0f;
            pid_obj["i"]          = (float) fanPid->i / 100.0f;
            pid_obj["d"]          = (float) fanPid->d / 100.0f;
        }
    }
    doc["invertFanPolarity"] = board->isInvertFanPolarityEnabled() ? 1 : 0;
#if defined(NERDAXE) || defined(NERDAXEGAMMA)
    doc["pidUseMax"]         = Config::isFanPidUseMax();
#endif

    // --- network ---
    {
        char *hostname = Config::getHostname();
        char *ssid     = Config::getWifiSSID();
        doc["hostname"] = hostname;
        doc["ssid"]     = ssid;
        free(hostname);
        free(ssid);
    }

    // --- mempool ---
    {
        doc["mempoolCustom"] = Config::isMempoolCustom();
        char *mempoolUrl = Config::getMempoolUrl();
        doc["mempoolUrl"] = mempoolUrl;
        free(mempoolUrl);
    }

    // --- display ---
    doc["flipScreen"]    = board->isFlipScreenEnabled() ? 1 : 0;
    doc["invertScreen"]  = Config::isInvertScreenEnabled() ? 1 : 0;
    doc["autoScreenOff"] = Config::isAutoScreenOffEnabled() ? 1 : 0;

    return sendJsonResponse(req, doc);
}

// ---------------------------------------------------------------------------
// PATCH /api/v2/settings
// ---------------------------------------------------------------------------
// Accepts the same structure as GET /api/v2/settings (all fields optional).
// Pool settings use a pools[] array instead of flat fallback* prefixes.

esp_err_t PATCH_V2_settings(httpd_req_t *req)
{
    ConGuard g(http_server, req);

    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if (validateOTP(req) != ESP_OK) {
        return ESP_FAIL;
    }

    PSRAMAllocator allocator;
    JsonDocument doc(&allocator);

    esp_err_t err = getJsonData(req, doc);
    if (err != ESP_OK) {
        return err;
    }

    // --- network ---
    if (doc["ssid"].is<const char*>()) {
        Config::setWifiSSID(doc["ssid"].as<const char*>());
    }
    if (doc["wifiPass"].is<const char*>()) {
        Config::setWifiPass(doc["wifiPass"].as<const char*>());
    }
    if (doc["hostname"].is<const char*>()) {
        Config::setHostname(doc["hostname"].as<const char*>());
    }

    // --- ASIC settings ---
    if (doc["coreVoltage"].is<uint16_t>()) {
        uint16_t v = doc["coreVoltage"].as<uint16_t>();
        if (v > 0) Config::setAsicVoltage(v);
    }
    if (doc["frequency"].is<uint16_t>()) {
        uint16_t f = doc["frequency"].as<uint16_t>();
        if (f > 0) Config::setAsicFrequency(f);
    }
    if (doc["jobInterval"].is<uint16_t>()) {
        uint16_t ji = doc["jobInterval"].as<uint16_t>();
        if (ji > 0) Config::setAsicJobInterval(ji);
    }
    if (doc["stratumDifficulty"].is<uint32_t>()) {
        Config::setStratumDifficulty(doc["stratumDifficulty"].as<uint32_t>());
    }
    if (doc["vrFrequency"].is<uint32_t>()) {
        Config::setVrFrequency(doc["vrFrequency"].as<uint32_t>());
    }

    // --- display ---
    if (doc["flipScreen"].is<bool>()) {
        Config::setFlipScreen(doc["flipScreen"].as<bool>());
    }
    if (doc["invertScreen"].is<bool>()) {
        Config::setInvertScreen(doc["invertScreen"].as<bool>());
    }
    if (doc["autoScreenOff"].is<bool>()) {
        Config::setAutoScreenOff(doc["autoScreenOff"].as<bool>());
    }
    if (doc["mempoolCustom"].is<bool>()) {
        Config::setMempoolCustom(doc["mempoolCustom"].as<bool>());
    }
    if (doc["mempoolUrl"].is<const char*>()) {
        Config::setMempoolUrl(doc["mempoolUrl"].as<const char*>());
    }
    if (doc["invertFanPolarity"].is<bool>()) {
        Config::setFanPolarity(doc["invertFanPolarity"].as<bool>());
    }
#if defined(NERDAXE) || defined(NERDAXEGAMMA)
    if (doc["pidUseMax"].is<bool>()) {
        Config::setFanPidUseMax(doc["pidUseMax"].as<bool>());
    }
#endif

    // --- misc ---
    if (doc["stratumKeep"].is<bool>() || doc["stratumKeep"].is<int>()) {
        bool value = doc["stratumKeep"].as<int>() != 0;
        Config::setStratumKeepaliveEnabled(value);
    }
    if (doc["canMaster"].is<bool>() || doc["canMaster"].is<int>()) {
        bool value = doc["canMaster"].as<int>() != 0;
        Config::setCanEnabled(value);
    }

    // --- fans[] ---
    if (doc["fans"].is<JsonArray>()) {
        JsonArray fans = doc["fans"].as<JsonArray>();
        int ch = 0;
        for (JsonObject fan : fans) {
            if (ch > 1) break;
            if (fan["mode"].is<uint16_t>())
                Config::setFanMode(ch, fan["mode"].as<uint16_t>());
            if (fan["manualSpeed"].is<uint16_t>())
                Config::setFanManualSpeed(ch, fan["manualSpeed"].as<uint16_t>());
            if (fan["overheatTemp"].is<uint16_t>())
                Config::setFanOverheatTemp(ch, fan["overheatTemp"].as<uint16_t>());
            if (fan["pid"].is<JsonObject>()) {
                JsonObject p = fan["pid"].as<JsonObject>();
                if (p["targetTemp"].is<uint16_t>())
                    Config::setFanPidTargetTemp(ch, p["targetTemp"].as<uint16_t>());
                if (p["p"].is<float>())
                    Config::setFanPidP(ch, (uint16_t)(p["p"].as<float>() * 100.0f));
                if (p["i"].is<float>())
                    Config::setFanPidI(ch, (uint16_t)(p["i"].as<float>() * 100.0f));
                if (p["d"].is<float>())
                    Config::setFanPidD(ch, (uint16_t)(p["d"].as<float>() * 100.0f));
            }
            ch++;
        }
    }

    // --- pools[] ---
    if (doc["poolMode"].is<uint16_t>()) {
        Config::setPoolMode(doc["poolMode"].as<uint16_t>());
    }
    if (doc["poolBalance"].is<uint16_t>()) {
        Config::setPoolBalance(doc["poolBalance"].as<uint16_t>());
    }

    bool verifyChanged[MAX_POOLS] = {};

    if (doc["pools"].is<JsonArray>()) {
        JsonArray pools = doc["pools"].as<JsonArray>();

        for (int i = 0; i < (int)pools.size() && i < MAX_POOLS; i++) {
            JsonObject pool = pools[i].as<JsonObject>();

            if (pool["url"].is<const char*>())             Config::setPoolURL(i, pool["url"].as<const char*>());
            if (pool["port"].is<uint16_t>())               Config::setPoolPort(i, pool["port"].as<uint16_t>());
            if (pool["user"].is<const char*>())            Config::setPoolUser(i, pool["user"].as<const char*>());
            if (pool["password"].is<const char*>())        Config::setPoolPass(i, pool["password"].as<const char*>());
            if (pool["enonceSubscribe"].is<bool>())        Config::setPoolEnonceSub(i, pool["enonceSubscribe"].as<bool>());
            if (pool["tls"].is<bool>())                    Config::setPoolTLS(i, pool["tls"].as<bool>());
            if (pool["protocol"].is<uint16_t>())           Config::setPoolProtocol(i, pool["protocol"].as<uint16_t>());
            if (pool["sv2AuthorityPubkey"].is<const char*>()) Config::setPoolSV2AuthorityPubkey(i, pool["sv2AuthorityPubkey"].as<const char*>());
            if (pool["sv2ChannelType"].is<uint16_t>())     Config::setPoolSV2ChannelType(i, pool["sv2ChannelType"].as<uint16_t>());
            if (pool["weight"].is<uint16_t>())             Config::setPoolWeight(i, pool["weight"].as<uint16_t>());

            // Coinbase verification is only stored for pools 0/1.
            if (i < 2) {
                if (pool["coinbaseVerifyMode"].is<uint16_t>()) {
                    verifyChanged[i] |= Config::getCoinbaseVerifyMode(i) != pool["coinbaseVerifyMode"].as<uint16_t>();
                    Config::setCoinbaseVerifyMode(i, pool["coinbaseVerifyMode"].as<uint16_t>());
                }
                if (pool["coinbaseMaxFee"].is<float>()) {
                    uint16_t newVal = (uint16_t)roundf(pool["coinbaseMaxFee"].as<float>() * 10.0f);
                    verifyChanged[i] |= Config::getCoinbaseMaxFee(i) != newVal;
                    Config::setCoinbaseMaxFee(i, newVal);
                }
                if (pool["coinbaseVerifyForce"].is<bool>()) {
                    verifyChanged[i] |= Config::getCoinbaseVerifyForce(i) != pool["coinbaseVerifyForce"].as<bool>();
                    Config::setCoinbaseVerifyForce(i, pool["coinbaseVerifyForce"].as<bool>());
                }
            }
        }
    }

    // Re-run verification for changed pools
    for (int i = 0; i < 2; i++) {
        if (verifyChanged[i]) {
            STRATUM_MANAGER->clearVerifyBlocked(i);
            STRATUM_MANAGER->resetVerificationStats(i);
        }
        STRATUM_MANAGER->rerunVerification(i);
    }
    if (SYSTEM_MODULE.getBoardError() == Board::Error::COINBASE_VERIFY_FAULT &&
        !STRATUM_MANAGER->isVerifyBlocked(0) && !STRATUM_MANAGER->isVerifyBlocked(1)) {
        SYSTEM_MODULE.clearBoardError();
    }

    doc.clear();

    Config::flush();

    httpd_resp_send_chunk(req, NULL, 0);

    // Reload all subsystems
    Board *board = SYSTEM_MODULE.getBoard();
    board->loadSettings();
    POWER_MANAGEMENT_MODULE.getFanController().loadSettings();
    SYSTEM_MODULE.loadSettings();
    STRATUM_MANAGER->loadSettings();

    return ESP_OK;
}
