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

/** Fewest ASICs a board needs before Triple and Quad are offered. */
export const MULTI_POOL_MIN_ASICS = 4;

/**
 * Presets to offer on a board with this many ASICs.
 *
 * Single, Failover and Dual are always available - dual-pool mining ships today
 * on single-ASIC hardware. Triple and Quad are held back below
 * MULTI_POOL_MIN_ASICS: the split is job-level, so a lone chip *can* drive four
 * pools, but a ~1 TH/s board split four ways leaves each pool ~250 GH/s, too
 * little for pool vardiff to settle on. In this tree only nerdaxe and
 * nerdaxegamma have one ASIC; every other board has 4, 6, 8 or 12.
 *
 * `current` is always included even when gated, so a board that already holds a
 * Triple or Quad configuration still shows its real state instead of a blank
 * control.
 */
export function availablePresets(asicCount: number, current: PoolPreset): PoolPreset[] {
  const presets = [PoolPreset.SINGLE, PoolPreset.FAILOVER, PoolPreset.DUAL];
  const multiPoolOk = asicCount >= MULTI_POOL_MIN_ASICS;

  if (multiPoolOk || current === PoolPreset.TRIPLE) {
    presets.push(PoolPreset.TRIPLE);
  }
  if (multiPoolOk || current === PoolPreset.QUAD) {
    presets.push(PoolPreset.QUAD);
  }
  return presets;
}

/** Translation key for a preset's dropdown label. */
export function presetLabelKey(preset: PoolPreset): string {
  switch (preset) {
    case PoolPreset.SINGLE:
      return 'SETTINGS.POOL_MODE_SINGLE';
    case PoolPreset.FAILOVER:
      return 'SETTINGS.POOL_MODE_FAILOVER';
    case PoolPreset.DUAL:
      return 'SETTINGS.POOL_MODE_DUAL';
    case PoolPreset.TRIPLE:
      return 'SETTINGS.POOL_MODE_TRIPLE';
    case PoolPreset.QUAD:
      return 'SETTINGS.POOL_MODE_QUAD';
  }
}
