/**
 * Pool preset <-> (poolMode, pool count) mapping.
 *
 * The settings dropdown is a derived view, not stored state: there is no NVS
 * key for "preset". The authoritative values are poolMode (0 = FAILOVER,
 * 1 = DUAL) and the number of configured pools, and the dropdown is computed
 * from them. Keeping a separate stored mode would create two sources of truth
 * that can disagree - the same failure class as the ghost-pool bug in 0458f37.
 *
 * Single Pool deliberately maps to FAILOVER rather than DUAL: the fallback
 * manager uses getAsicMinDifficulty() (512) while the dual manager uses
 * getAsicMinDifficultyDualPool() (256), and a lone pool under DUAL halves the
 * ASIC difficulty threshold for no benefit.
 */

export enum PoolPreset {
  SINGLE = 'single',
  FAILOVER = 'failover',
  DUAL = 'dual',
  TRIPLE = 'triple',
  QUAD = 'quad',
}

export interface PoolPresetConfig {
  /** 0 = FAILOVER, 1 = DUAL. Matches StratumManager::PoolMode. */
  poolMode: number;
  /** How many pool slots the form should show. */
  poolCount: number;
}

/**
 * Nearest preset for a stored configuration. Never rewrites anything - a
 * config that does not match a preset exactly (legacy DUAL with one pool, or
 * FAILOVER with extra slots stored) still resolves to its closest entry.
 */
export function presetFor(poolMode: number, poolCount: number): PoolPreset {
  if (poolCount <= 1) {
    return PoolPreset.SINGLE;
  }
  if (poolMode === 0) {
    return PoolPreset.FAILOVER;
  }
  if (poolCount === 2) {
    return PoolPreset.DUAL;
  }
  if (poolCount === 3) {
    return PoolPreset.TRIPLE;
  }
  return PoolPreset.QUAD;
}

/** The poolMode and slot count a preset selects. */
export function configFor(preset: PoolPreset): PoolPresetConfig {
  switch (preset) {
    case PoolPreset.SINGLE:
      return { poolMode: 0, poolCount: 1 };
    case PoolPreset.FAILOVER:
      return { poolMode: 0, poolCount: 2 };
    case PoolPreset.DUAL:
      return { poolMode: 1, poolCount: 2 };
    case PoolPreset.TRIPLE:
      return { poolMode: 1, poolCount: 3 };
    case PoolPreset.QUAD:
      return { poolMode: 1, poolCount: 4 };
  }
}

/**
 * Human-readable names of the pools lost by shrinking from storedCount to
 * targetCount, in order. Empty when nothing is dropped.
 *
 * Pure so it can be tested directly rather than re-implemented in a test
 * double: these are the names shown in the confirmation prompt, and getting
 * them wrong means telling someone the wrong pool is about to be erased.
 */
export function droppedPoolNames(targetCount: number, storedCount: number): string[] {
  const dropped: string[] = [];
  for (let i = targetCount; i < storedCount; i++) {
    dropped.push(`Pool ${i + 1}`);
  }
  return dropped;
}
