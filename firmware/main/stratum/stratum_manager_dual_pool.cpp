#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include "esp_log.h"
#include "global_state.h"
#include "create_jobs_task.h"
#include "macros.h"
#include "nvs_config.h"
#include "stratum_manager_dual_pool.h"
#include "utils.h"
#include "utils.h"

StratumManagerDualPool::StratumManagerDualPool() : StratumManager(PoolMode::DUAL)
{
    // NOP
}

void StratumManagerDualPool::reconnectTimerCallback(int index)
{
    PThreadGuard lock(m_mutex);

    // verify-blocked pool must not reconnect until the block is cleared
    if (isVerifyBlocked(index)) return;

    // dual pool: both pools always try to stay alive
    m_stratumTasks[index]->connect();
}

void StratumManagerDualPool::connectedCallback(int index)
{
    PThreadGuard lock(m_mutex);

    // Blocked pool connected (e.g. after settings save) — kick it off immediately
    if (isVerifyBlocked(index)) {
        m_stratumTasks[index]->disconnect();
        return;
    }

    // reset poolDiffErr
    if (index >= 0 && index < MAX_POOLS) {
        m_poolDiffErr[index] = false;
    }
}

void StratumManagerDualPool::disconnectedCallback(int index)
{
    PThreadGuard lock(m_mutex);
    create_job_invalidate(index);
    m_stratumTasks[index]->m_validNotify = false;
    m_stratumTasks[index]->startReconnectTimer();
}

int StratumManagerDualPool::poolWeight(int i)
{
    if (i < 0 || i >= m_numPools) {
        return 0;
    }
    // N<=2: keep the classic balance split (pool0 = balance, pool1 = 100-balance)
    // so the existing 2-pool slider behaves exactly as before.
    if (m_numPools <= 2) {
        if (i == 0) return m_balance;
        if (i == 1) return 100 - m_balance;
        return 0;
    }
    // N>2: configurable per-pool weights (cached from NVS in loadSettings).
    return m_poolWeights[i];
}

void StratumManagerDualPool::resetScheduler()
{
    for (int i = 0; i < MAX_POOLS; i++) {
        m_swrr_current[i] = 0;
    }
}

int StratumManagerDualPool::getNextActivePool()
{
    PThreadGuard lock(m_mutex);

    // Smooth weighted round-robin over the pools that currently have valid work
    // and a positive weight. Each call advances every eligible pool's credit by
    // its weight, picks the highest, then debits the picked pool by the total
    // eligible weight. This is proportional, deterministic, and self-balances
    // when a pool drops (its share is redistributed to the survivors). At N=2 it
    // yields the same PRIMARY/SECONDARY ratio as the old Bresenham dither.
    int best = -1;
    int total = 0;

    for (int i = 0; i < m_numPools; i++) {
        bool valid = m_stratumTasks[i] && m_stratumTasks[i]->m_validNotify && !isVerifyBlocked(i);
        int w = poolWeight(i);
        if (!valid || w <= 0) {
            continue;
        }
        m_swrr_current[i] += w;
        total += w;
        if (best < 0 || m_swrr_current[i] > m_swrr_current[best]) {
            best = i;
        }
    }

    if (best < 0) {
        // No weighted+valid pool. Fall back to any pool that at least has valid
        // work (e.g. all weights 0), else PRIMARY. Keep credits from drifting.
        resetScheduler();
        for (int i = 0; i < m_numPools; i++) {
            bool valid = m_stratumTasks[i] && m_stratumTasks[i]->m_validNotify && !isVerifyBlocked(i);
            if (valid) return i;
        }
        return PRIMARY;
    }

    m_swrr_current[best] -= total;
    return best;
}

const char *StratumManagerDualPool::getPoolHost(int pool)
{
    if (!m_stratumConfig[pool]) {
        return "-";
    }
    return m_stratumConfig[pool]->getHost();
}

int StratumManagerDualPool::getPoolPort(int pool)
{
    if (!m_stratumConfig[pool]) {
        return 0;
    }
    return m_stratumConfig[pool]->getPort();
}

uint64_t StratumManagerDualPool::getSharesAccepted(int pool) {
    return m_accepted[pool];
}

uint64_t StratumManagerDualPool::getSharesRejected(int pool) {
    return m_rejected[pool];
}

uint32_t StratumManagerDualPool::selectAsicDiff(int pool, uint32_t poolDiff)
{
    Board *board = SYSTEM_MODULE.getBoard();
    uint32_t asicMax = board->getAsicMaxDifficulty();
    uint32_t asicMin = board->getAsicMinDifficultyDualPool();

    static uint32_t poolDiffs[MAX_POOLS] = {0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu};

    // shouldn't happen
    if (pool < 0 || pool >= MAX_POOLS) {
        return asicMax;
    }

    m_poolDiffErr[pool] = poolDiff < asicMin;

    poolDiffs[pool] = poolDiff;

    // ASIC difficulty must satisfy every pool we actually send work to -> take
    // the min over pools with a positive weight (a zero-weight pool gets no jobs,
    // so its difficulty must not drag the ASIC target down).
    uint32_t minDiff = 0xffffffffu;
    for (int i = 0; i < m_numPools; i++) {
        if (poolWeight(i) <= 0) continue;
        minDiff = std::min(minDiff, poolDiffs[i]);
    }
    if (minDiff == 0xffffffffu) {
        return asicMax; // no weighted pool reported a diff yet
    }

    // clamp to ASIC range
    if (minDiff < asicMin) {
        return asicMin;
    }
    if (minDiff > asicMax) {
        return asicMax;
    }
    return minDiff;
}

bool StratumManagerDualPool::acceptsNotifyFrom(int pool)
{
    return true;
}

int StratumManagerDualPool::getActivePoolBalance(int pool)
{
    if (pool < 0 || pool >= m_numPools || !isConnected(pool)) {
        return 0;
    }

    // A pool's share of the hashrate = its weight over the sum of the weights of
    // all currently-connected pools. Reduces to the balance split at N=2.
    int total = 0;
    for (int i = 0; i < m_numPools; i++) {
        if (isConnected(i)) {
            total += poolWeight(i);
        }
    }
    if (total <= 0) {
        return 0;
    }
    return (100 * poolWeight(pool)) / total;
}

float StratumManagerDualPool::getActivePoolHashrate(int pool)
{
    int balance = getActivePoolBalance(pool);
    return SYSTEM_MODULE.getCurrentHashrate() * (float) balance / 100.0f;
}

void StratumManagerDualPool::loadSettings()
{
    PThreadGuard lock(m_mutex);

    uint16_t newBalance = Config::getPoolBalance();

    bool reconnect = false;
    if (m_balance != newBalance) {
        m_balance = newBalance;
        resetScheduler();
        reconnect = true;
    }

    // Refresh per-pool weights (N>2 split). A weight change only needs the SWRR
    // credits reset so it re-converges to the new ratio; no reconnect required.
    for (int i = 0; i < MAX_POOLS; i++) {
        int w = (int) Config::getPoolWeight(i);
        if (m_poolWeights[i] != w) {
            m_poolWeights[i] = w;
            resetScheduler();
        }
    }

    StratumManager::loadSettings(reconnect);
};

void StratumManagerDualPool::saveSettings(const JsonDocument &doc) {
    PThreadGuard lock(m_mutex);

    if (doc["poolBalance"].is<uint16_t>()) {
        Config::setPoolBalance(doc["poolBalance"].as<uint16_t>());
    }
    StratumManager::saveSettings(doc);
    Config::flush();
}

void StratumManagerDualPool::checkForBestDiff(int pool, double diff, uint32_t nbits)
{
    PThreadGuard lock(m_mutex);

    if (pool < 0 || pool >= MAX_POOLS) {
        return;
    }

    if ((uint64_t) diff > m_bestSessionDiff[pool]) {
        m_bestSessionDiff[pool] = (uint64_t) diff;
        uint64_t best = 0;
        for (int i = 0; i < m_numPools; i++) {
            best = std::max(best, m_bestSessionDiff[i]);
        }
        suffixString(best, m_bestSessionDiffString, DIFF_STRING_SIZE, 0);
    }

    StratumManager::checkForBestDiff(pool, diff, nbits);
}

void StratumManagerDualPool::getManagerInfoJson(JsonObject &obj)
{
    PThreadGuard lock(m_mutex);

    StratumManager::getManagerInfoJson(obj);

    JsonArray arr = obj["pools"].to<JsonArray>();

    for (int i = 0; i < m_numPools; i++) {
        JsonObject pool = arr.add<JsonObject>();

        pool["active"] = true; // dual pool mode: all configured pools mine

        pool["url"]    = m_stratumConfig[i] ? m_stratumConfig[i]->getHost() : "";
        pool["port"]   = m_stratumConfig[i] ? m_stratumConfig[i]->getPort() : 0;
        pool["user"]   = m_stratumConfig[i] ? m_stratumConfig[i]->getUser() : "";
        pool["weight"] = poolWeight(i);

        pool["connected"] = m_stratumTasks[i] ? m_stratumTasks[i]->m_isConnected : false;
        pool["verifyBlocked"] = getVerifyBlockedReason(i) ? getVerifyBlockedReason(i) : "";
        pool["poolDifficulty"] = m_poolDifficulty[i];
        pool["networkDifficulty"] = m_networkDifficulty[i];
        pool["poolDiffErr"] = m_poolDiffErr[i];
        pool["accepted"] = m_accepted[i];
        pool["rejected"] = m_rejected[i];
        pool["pingRtt"]  = m_pingTasks[i] ? m_pingTasks[i]->get_last_ping_rtt() : 0;
        pool["pingLoss"] = m_pingTasks[i] ? m_pingTasks[i]->get_recent_ping_loss() : 0;
        pool["bestDiff"] = m_bestSessionDiff[i];
        pool["activeProtocol"] = m_stratumConfig[i] ? (int)m_stratumConfig[i]->getProtocol() : 0;
        pool["encrypted"] = m_stratumConfig[i] ? (m_stratumConfig[i]->isSV2() || m_stratumConfig[i]->isTLS()) : false;
    }
}
