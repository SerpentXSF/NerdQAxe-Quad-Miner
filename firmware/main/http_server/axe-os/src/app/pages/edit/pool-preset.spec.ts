import { PoolPreset, configFor, droppedPoolNames, presetFor } from './pool-preset';

describe('pool-preset mapping', () => {
  it('maps failover mode by configured count', () => {
    expect(presetFor(0, 1)).toBe(PoolPreset.SINGLE);
    expect(presetFor(0, 2)).toBe(PoolPreset.FAILOVER);
  });

  it('maps dual mode by configured count', () => {
    expect(presetFor(1, 2)).toBe(PoolPreset.DUAL);
    expect(presetFor(1, 3)).toBe(PoolPreset.TRIPLE);
    expect(presetFor(1, 4)).toBe(PoolPreset.QUAD);
  });

  it('shows legacy dual-mode-with-one-pool as Single', () => {
    expect(presetFor(1, 1)).toBe(PoolPreset.SINGLE);
  });

  it('shows legacy failover-with-extra-pools as Failover', () => {
    expect(presetFor(0, 3)).toBe(PoolPreset.FAILOVER);
    expect(presetFor(0, 4)).toBe(PoolPreset.FAILOVER);
  });

  it('treats a zero or negative count as Single', () => {
    expect(presetFor(0, 0)).toBe(PoolPreset.SINGLE);
    expect(presetFor(1, 0)).toBe(PoolPreset.SINGLE);
  });

  it('clamps a count above MAX_POOLS to Quad', () => {
    expect(presetFor(1, 5)).toBe(PoolPreset.QUAD);
  });

  it('gives each preset the documented poolMode and slot count', () => {
    expect(configFor(PoolPreset.SINGLE)).toEqual({ poolMode: 0, poolCount: 1 });
    expect(configFor(PoolPreset.FAILOVER)).toEqual({ poolMode: 0, poolCount: 2 });
    expect(configFor(PoolPreset.DUAL)).toEqual({ poolMode: 1, poolCount: 2 });
    expect(configFor(PoolPreset.TRIPLE)).toEqual({ poolMode: 1, poolCount: 3 });
    expect(configFor(PoolPreset.QUAD)).toEqual({ poolMode: 1, poolCount: 4 });
  });

  it('round-trips every preset through configFor and back', () => {
    for (const preset of Object.values(PoolPreset)) {
      const cfg = configFor(preset);
      expect(presetFor(cfg.poolMode, cfg.poolCount)).toBe(preset);
    }
  });
});

describe('droppedPoolNames', () => {
  it('returns nothing when the count is unchanged or growing', () => {
    expect(droppedPoolNames(2, 2)).toEqual([]);
    expect(droppedPoolNames(4, 2)).toEqual([]);
  });

  it('names a single dropped pool', () => {
    expect(droppedPoolNames(2, 3)).toEqual(['Pool 3']);
  });

  it('names two dropped pools', () => {
    expect(droppedPoolNames(2, 4)).toEqual(['Pool 3', 'Pool 4']);
  });

  it('names three dropped pools when going down to one', () => {
    expect(droppedPoolNames(1, 4)).toEqual(['Pool 2', 'Pool 3', 'Pool 4']);
  });
});
