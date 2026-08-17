#pragma once
#include "stratum_config.h"
#include "ArduinoJson.h"
#include "esp_log.h"
#include "nvs_config.h"
#include "psram_allocator.h"

static const char *TAG = "StratumConfig";

static bool strEq(const char *a, const char *b)
{
    if (a == b)
        return true;
    if (!a || !b)
        return false;
    return strcmp(a, b) == 0;
}

StratumConfig::StratumConfig(int pool)
{
    m_index     = pool;
    m_host      = Config::getPoolURL(pool);
    m_port      = Config::getPoolPort(pool);
    m_user      = Config::getPoolUser(pool);
    m_password  = Config::getPoolPass(pool);
    m_enonceSub = Config::getPoolEnonceSub(pool);
    m_tls       = Config::getPoolTLS(pool);
    m_protocol  = (StratumProtocol) Config::getPoolProtocol(pool);
}

bool StratumConfig::reload()
{
    // Load new values
    char *newHost = Config::getPoolURL(m_index);
    int newPort   = Config::getPoolPort(m_index);
    char *newUser = Config::getPoolUser(m_index);
    char *newPass = Config::getPoolPass(m_index);
    bool newEnsub = Config::getPoolEnonceSub(m_index);
    bool newTLS   = Config::getPoolTLS(m_index);
    StratumProtocol newProto = (StratumProtocol) Config::getPoolProtocol(m_index);
    // Compare
    bool same =
        strEq(m_host, newHost) &&
        m_port == newPort &&
        strEq(m_user, newUser) &&
        strEq(m_password, newPass) &&
        m_enonceSub == newEnsub &&
        m_tls == newTLS &&
        m_protocol == newProto;

    if (same) {
        // Free temporary values (they were newly allocated by Config::get)
        safe_free(newHost);
        safe_free(newUser);
        safe_free(newPass);
        return false;
    }

    // Update fields: first free old values
    safe_free(m_host);
    safe_free(m_user);
    safe_free(m_password);

    m_host       = newHost;
    m_port       = newPort;
    m_user       = newUser;
    m_password   = newPass;
    m_enonceSub  = newEnsub;
    m_tls        = newTLS;
    m_protocol   = newProto;

    return true;
}

void StratumConfig::copyInto(StratumConfig *dst)
{
    safe_free(dst->m_host);
    safe_free(dst->m_user);
    safe_free(dst->m_password);

    dst->m_index     = m_index;
    dst->m_host      = m_host ? strdup(m_host) : nullptr;
    dst->m_port      = m_port;
    dst->m_user      = m_user ? strdup(m_user) : nullptr;
    dst->m_password  = m_password ? strdup(m_password) : nullptr;
    dst->m_enonceSub = m_enonceSub;
    dst->m_tls       = m_tls;
    dst->m_protocol  = m_protocol;
}


/*
void StratumConfig::toLog(const StratumConfig &cfg, const char* prefix) {
    char c = (cfg.m_index == 0) ? 'P' : 'S';
    ESP_LOGE(TAG, "%s [%c] host: %s", prefix, c, cfg.m_host ? cfg.m_host : "null");
    ESP_LOGE(TAG, "%s [%c] port: %d", prefix, c, cfg.m_port);
    ESP_LOGE(TAG, "%s [%c] user: %s", prefix, c, cfg.m_user ? cfg.m_user : "null");
    ESP_LOGE(TAG, "%s [%c] pass: %s", prefix, c, cfg.m_password ? cfg.m_password : "null");
    ESP_LOGE(TAG, "%s [%c] esub:  %s", prefix, c, cfg.m_enonceSub ? "true" : "false");
}
*/