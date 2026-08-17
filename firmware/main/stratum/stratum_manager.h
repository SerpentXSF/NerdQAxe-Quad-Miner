#pragma once
#include <pthread.h>

#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "lwip/inet.h"
#include "esp_log.h"
#include "ArduinoJson.h"

#include "coinbase_decoder.h"
#include "stratum_task.h"
#include "../tasks/ping_task.h"

#define DIFF_STRING_SIZE 12

// Maximum number of pools the miner can drive at once. 2 = classic primary+fallback
// / dual-pool; raised to 4 for the quad-pool (per-ASIC-capable) work. Kept a power of
// two but indices are bounds-checked (poolIndex), not masked, so non-power-of-two is
// also safe later. Defined in nvs_config.h (single source of truth); guarded here so
// stratum_manager.h stays self-contained if included without it.
#ifndef MAX_POOLS
#define MAX_POOLS 4
#endif

// Clamp a pool index to the valid range. Replaces the old `pool & 1` mask (which
// silently wrapped index 2 -> 0 / 3 -> 1 once more than two pools existed); an
// out-of-range index now falls back to pool 0 instead of corrupting a neighbour's slot.
static inline int poolIndex(int pool)
{
    return (pool >= 0 && pool < MAX_POOLS) ? pool : 0;
}

/**
 * @brief StratumManager handles pool selection, connection management, and failover.
 */
class StratumTaskV2;

class StratumManager {
    friend StratumTaskBase;
    friend StratumTaskV1;
    friend StratumTaskV2;
    friend PingTask;
  public:
    enum Selected
    {
        PRIMARY = 0,
        SECONDARY = 1
    };

    enum PoolMode
    {
        FAILOVER = 0,
        DUAL = 1
    };

  protected:

    const char *m_tag = "stratum-manager"; ///< Debug tag for logging

    pthread_mutex_t m_mutex = PTHREAD_MUTEX_INITIALIZER; ///< Mutex for thread safety
    StratumApiV1Message m_stratum_api_v1_message;        ///< API message handler
    PoolMode m_poolmode;                                 // default FAILOVER

    // Number of pools this manager actually drives (tasks/configs created for
    // indices 0..m_numPools-1). DUAL grows this to the count of contiguously
    // configured pools (2..MAX_POOLS); FAILOVER stays at 2 (its state machine is
    // primary+secondary only). Computed once at construction.
    int m_numPools = 2;
    int computeNumPools();
    uint64_t m_lastSubmitResponseTimestamp = 0;              ///< Timestamp of last submitted share response

    StratumTaskBase *m_stratumTasks[MAX_POOLS]{};                    ///< Primary and secondary Stratum tasks
    PingTask *m_pingTasks[MAX_POOLS]{};
    StratumConfig *m_stratumConfig[MAX_POOLS]{};

    uint32_t m_totalFoundBlocks = 0;
    uint32_t m_foundBlocks = 0;

    // Difficulty tracking (compatibility)
    uint64_t m_totalBestDiff = 0;                       // Best nonce difficulty found
    char m_totalBestDiffString[DIFF_STRING_SIZE]{};        // String representation of the best difficulty
    char m_bestSessionDiffString[DIFF_STRING_SIZE]{}; // String representation of the best session difficulty

    bool m_initialized = false;

    // Coinbase decoder: extranonce state per pool + decoded result + verification
    char *m_extranonce1[MAX_POOLS]{};
    int m_extranonce2_len[MAX_POOLS]{};
    coinbase_result_t m_coinbaseResult[MAX_POOLS]{};
    bool m_verificationOk[MAX_POOLS]{};
    uint32_t m_verificationFailCount[MAX_POOLS]{};
    uint32_t m_verificationCheckCount[MAX_POOLS]{};
    // "" = not blocked, "address_not_found", "fee_exceeded"
    const char *m_verifyBlockedReason[MAX_POOLS]{};

    void processCoinbase(int pool, const mining_notify *notify);
    void processCoinbase(int pool, const char *coinbase_1_hex, const char *coinbase_2_hex,
                         uint32_t version, uint32_t nbits,
                         const char *extranonce1_hex, int extranonce2_len);
    void storeExtranonce(int pool, const char *extranonce, int extranonce2_len);
    void runVerification(int pool);

    PoolMode getPoolMode() const
    {
        return m_poolmode;
    }

    void copyConfigInto(int pool, StratumConfig *dst);

    // Helper methods for connection management
    void connect(int index);     ///< Connect to a specified pool (0 = primary, 1 = secondary)
    void disconnect(int index);  ///< Disconnect from a specified pool
    bool isConnected(int index); ///< Check if a pool is connected

    // Handles incoming Stratum responses
    void dispatch(int pool, JsonDocument &doc);

    // Factory method for creating protocol-specific tasks
    virtual StratumTaskBase* createTask(int index);

    // Core Stratum management task
    void task();

    // Clears queued mining jobs
    void cleanQueue();

    void freeStratumV1Message(StratumApiV1Message *message);

    // abstract methods
    // reconnect logic for failover mode
    virtual void reconnectTimerCallback(int index) = 0;
    virtual void connectedCallback(int index) = 0;
    virtual void disconnectedCallback(int index) = 0;
    virtual bool acceptsNotifyFrom(int pool) = 0;

    virtual void acceptedShare(int pool) = 0;
    virtual void rejectedShare(int pool) = 0;
    virtual void setPoolDifficulty(int pool, uint32_t diff) = 0;
    virtual void setNetworkDifficulty(int pool, uint32_t nbits) {}

    virtual int getPoolMode() = 0;

    // reset stats for a single pool (called from loadSettings with mutex held)
    virtual void resetPoolSessionStats(int pool) {
    }

  public:
    virtual void resetSessionStats() {
        PThreadGuard lock(m_mutex);
        m_foundBlocks = 0;
    }
    StratumManager(PoolMode mode);
    static void taskWrapper(void *pvParameters); ///< Wrapper function for task execution

    // Submit shares to the active Stratum pool
    // version_rolled = full rolled version (base | rolled bits)
    // version_base   = original block template version
    void submitShare(int pool, const char *jobid, const char *extranonce_2, const uint32_t ntime, const uint32_t nonce,
                     const uint32_t version_rolled, const uint32_t version_base);

    void checkForFoundBlock(int pool, double diff, uint32_t nbits);

    bool isAnyConnected();
    int getNumConnectedPools();
    int getNumPools() const { return m_numPools; }

    void reconnectAll();

    virtual bool isDualPool() const { return false; }
    virtual bool isFallback() const { return false; }

    virtual void getManagerInfoJson(JsonObject &obj);

    virtual void loadSettings() = 0;
    virtual void loadSettings(bool reconnect);
    virtual void saveSettings(const JsonDocument &doc);

    virtual bool isUsingFallback() {
        return false;
    }

    // abstract
    virtual const char *getResolvedIpForPool(int pool) const;

    virtual int getNextActivePool() = 0;

    virtual uint32_t selectAsicDiff(int pool, uint32_t poolDiff) = 0;

    virtual void checkForBestDiff(int pool, double diff, uint32_t nbits) = 0;

    bool isInitialized() {
        return m_initialized;
    }

    // compatibility
    virtual uint64_t getSharesAccepted() = 0;
    virtual uint64_t getSharesRejected() = 0;

    uint32_t getTotalFoundBlocks() {
        return m_totalFoundBlocks;
    }

    uint32_t getFoundBlocks() {
        return m_foundBlocks;
    }

    const char* getBestDiffString() {
        return m_totalBestDiffString;
    }

    uint64_t getBestDiff() {
        return m_totalBestDiff;
    }

    virtual uint64_t getBestSessionDiff() = 0;

    const char *getBestSessionDiffString() {
        return m_bestSessionDiffString;
    }

    virtual int getPoolErrors() = 0;

    virtual uint32_t getPoolDifficulty() = 0;
    virtual double getNetworkDifficulty() { return 0; }

    coinbase_result_t getCoinbaseResult(int pool) {
        PThreadGuard lock(m_mutex);
        return m_coinbaseResult[poolIndex(pool)];
    }

    void setCoinbaseResult(int pool, const coinbase_result_t &result) {
        PThreadGuard lock(m_mutex);
        m_coinbaseResult[poolIndex(pool)] = result;
        runVerification(poolIndex(pool));
    }

    bool getVerificationOk(int pool) {
        PThreadGuard lock(m_mutex);
        return m_verificationOk[poolIndex(pool)];
    }

    uint32_t getVerificationFailCount(int pool) {
        PThreadGuard lock(m_mutex);
        return m_verificationFailCount[poolIndex(pool)];
    }

    uint32_t getVerificationCheckCount(int pool) {
        PThreadGuard lock(m_mutex);
        return m_verificationCheckCount[poolIndex(pool)];
    }

    void rerunVerification(int pool) {
        PThreadGuard lock(m_mutex);
        runVerification(poolIndex(pool));
    }

    void resetVerificationStats(int pool) {
        PThreadGuard lock(m_mutex);
        m_verificationCheckCount[poolIndex(pool)] = 0;
        m_verificationFailCount[poolIndex(pool)] = 0;
    }

    bool isVerifyBlocked(int pool) const { return m_verifyBlockedReason[poolIndex(pool)] != nullptr; }
    const char *getVerifyBlockedReason(int pool) const { return m_verifyBlockedReason[poolIndex(pool)]; }

    void clearVerifyBlocked(int pool) {
        m_verifyBlockedReason[poolIndex(pool)] = nullptr;
    }

    virtual int getCompatPingPoolIndex() = 0;

    PingTask *getPingTask(int i) {
        return m_pingTasks[i];
    }
};

double get_last_ping_rtt();
double get_recent_ping_loss();
