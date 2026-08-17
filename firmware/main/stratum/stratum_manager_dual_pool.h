#pragma once

#include "stratum_manager.h"
#include "utils.h"

class StratumManagerDualPool : public StratumManager {
    friend StratumTaskBase; ///< Allows StratumTaskBase to access private members

  protected:
    int m_balance = 50;

    // Smooth weighted round-robin (SWRR) running credit, one per pool. Replaces
    // the old single Bresenham accumulator; reduces to the identical PRIMARY/
    // SECONDARY ratio at N=2. Bounded within [-sumWeights, +sumWeights].
    int32_t m_swrr_current[MAX_POOLS]{};

    // Per-pool scheduling weights (percent; only ratios matter), cached from
    // NVS in loadSettings. For N>2 these drive the split; for N<=2 the classic
    // poolBalance slider is used instead. Default 25 each = even split.
    int m_poolWeights[MAX_POOLS] = {25, 25, 25, 25};

    // Effective scheduling weight for a pool. N<=2 keeps the classic balance
    // split; N>2 uses the configurable per-pool weights above.
    int poolWeight(int i);
    void resetScheduler();

    uint64_t m_accepted[MAX_POOLS]{};
    uint64_t m_rejected[MAX_POOLS]{};
    uint64_t m_bestSessionDiff[MAX_POOLS]{};
    bool m_poolDiffErr[MAX_POOLS]{};
    uint32_t m_poolDifficulty[MAX_POOLS]{0};
    double m_networkDifficulty[MAX_POOLS]{0};

    virtual void reconnectTimerCallback(int index);
    virtual void connectedCallback(int index);
    virtual void disconnectedCallback(int index);

    virtual bool acceptsNotifyFrom(int pool);

    virtual void setPoolDifficulty(int pool, uint32_t diff) {
        m_poolDifficulty[pool] = diff;
    };

    virtual void setNetworkDifficulty(int pool, uint32_t nbits) {
        if (pool >= 0 && pool < MAX_POOLS && nbits != 0) {
            m_networkDifficulty[pool] = calculateNetworkDifficulty(nbits);
        }
    }

    virtual void acceptedShare(int pool)
    {
        m_accepted[pool]++;
    }

    virtual void rejectedShare(int pool)
    {
        m_rejected[pool]++;
    }

    virtual int getPoolMode() {
        return 1;
    }


  public:
    StratumManagerDualPool();

    bool getPoolDiffErr(int i) {
        if (i < 0 || i >= MAX_POOLS) {
            return false;
        }
        return m_poolDiffErr[i];
    }

    virtual const char *getPoolHost(int pool);
    virtual int getPoolPort(int pool);

    virtual uint32_t selectAsicDiff(int pool, uint32_t poolDiff);

    virtual int getNextActivePool();

    bool isDualPool() const override { return true; }

    virtual void checkForBestDiff(int pool, double diff, uint32_t nbits);

    virtual void getManagerInfoJson(JsonObject &obj);

    virtual void loadSettings();
    virtual void saveSettings(const JsonDocument &doc);

    virtual uint64_t getSharesAccepted(int pool);
    virtual uint64_t getSharesRejected(int pool);

    float getActivePoolHashrate(int pool);
    int getActivePoolBalance(int pool);

    // aggregated
    virtual uint64_t getSharesAccepted() {
        uint64_t sum = 0;
        for (int i = 0; i < m_numPools; i++) sum += m_accepted[i];
        return sum;
    }

    virtual uint64_t getSharesRejected() {
        uint64_t sum = 0;
        for (int i = 0; i < m_numPools; i++) sum += m_rejected[i];
        return sum;
    }

    virtual uint32_t getPoolDifficulty() {
        return (m_balance >= 50) ? m_poolDifficulty[0] : m_poolDifficulty[1];
    };

    virtual double getNetworkDifficulty() {
        return (m_balance >= 50) ? m_networkDifficulty[0] : m_networkDifficulty[1];
    }

    virtual void resetPoolSessionStats(int pool) override {
        StratumManager::resetPoolSessionStats(pool);
        m_accepted[pool] = 0;
        m_rejected[pool] = 0;
        m_bestSessionDiff[pool] = 0;
        uint64_t best = 0;
        for (int i = 0; i < m_numPools; i++) {
            best = std::max(best, m_bestSessionDiff[i]);
        }
        suffixString(best, m_bestSessionDiffString, DIFF_STRING_SIZE, 0);
        if (m_stratumTasks[pool]) m_stratumTasks[pool]->m_poolErrors = 0;
    }

    virtual void resetSessionStats() override {
        PThreadGuard lock(m_mutex);
        for (int i = 0; i < m_numPools; i++) {
            resetPoolSessionStats(i);
        }
    }

    virtual uint64_t getBestSessionDiff() {
        uint64_t best = 0;
        for (int i = 0; i < m_numPools; i++) {
            best = std::max(best, m_bestSessionDiff[i]);
        }
        return best;
    }

    virtual int getPoolErrors() {
        int errs = 0;
        for (int i = 0; i < m_numPools; i++) {
            if (m_stratumTasks[i]) errs += m_stratumTasks[i]->m_poolErrors;
        }
        return errs;
    }

    virtual int getCompatPingPoolIndex() {
        return (m_balance >= 50 || m_numPools < 2) ? 0 : 1;
    }
};
