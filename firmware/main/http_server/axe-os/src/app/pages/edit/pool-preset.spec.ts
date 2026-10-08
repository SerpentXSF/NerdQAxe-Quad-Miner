import {
  PoolPreset,
  availablePresets,
  configFor,
  droppedPoolNames,
  presetFor,
  presetLabelKey,
} from './pool-preset';

describe('availablePresets', () => {
  it('offers only Single, Failover and Dual on a single-ASIC board', () => {
    expect(availablePresets(1, PoolPreset.DUAL)).toEqual([
      PoolPreset.SINGLE, PoolPreset.FAILOVER, PoolPreset.DUAL,
    ]);
  });

  it('offers all five on a four-ASIC board', () => {
    expect(availablePresets(4, PoolPreset.QUAD)).toEqual([
      PoolPreset.SINGLE, PoolPreset.FAILOVER, PoolPreset.DUAL,
      PoolPreset.TRIPLE, PoolPreset.QUAD,
    ]);
  });

  it('offers all five on boards with more than four ASICs', () => {
    for (const asics of [6, 8, 12]) {
      expect(availablePresets(asics, PoolPreset.DUAL).length).toBe(5);
    }
  });

  it('still shows the current preset on a single-ASIC board already set to Quad', () => {
    const list = availablePresets(1, PoolPreset.QUAD);
    expect(list).toContain(PoolPreset.QUAD);
    expect(list).not.toContain(PoolPreset.TRIPLE);
  });

  it('still shows the current preset on a single-ASIC board already set to Triple', () => {
    const list = availablePresets(1, PoolPreset.TRIPLE);
    expect(list).toContain(PoolPreset.TRIPLE);
    expect(list).not.toContain(PoolPreset.QUAD);
  });

  it('never hides Single, Failover or Dual regardless of ASIC count', () => {
    for (const asics of [0, 1, 2, 4, 12]) {
      const list = availablePresets(asics, PoolPreset.SINGLE);
      expect(list).toContain(PoolPreset.SINGLE);
      expect(list).toContain(PoolPreset.FAILOVER);
      expect(list).toContain(PoolPreset.DUAL);
    }
  });
});

describe('presetLabelKey', () => {
  it('gives every preset a distinct SETTINGS translation key', () => {
    const keys = Object.values(PoolPreset).map(presetLabelKey);
    expect(new Set(keys).size).toBe(keys.length);
    for (const k of keys) {
      expect(k.startsWith('SETTINGS.POOL_MODE_')).toBe(true);
    }
  });
});

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
