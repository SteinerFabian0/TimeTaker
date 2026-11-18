/*
 * Small seeded random number generators for the simulator and the fixture
 * generator. xorshift32: fast, repeatable, good enough for lap-time noise.
 * Never used for anything that needs real randomness (the clientId uses
 * crypto.getRandomValues).
 */

export interface Rng {
  /** Uniform in [0, 1). */
  next(): number;
  /** Uniform in [lo, hi). */
  range(lo: number, hi: number): number;
  /** Integer in [lo, hi]. */
  int(lo: number, hi: number): number;
  /** Normal(mean, sd), Box–Muller. */
  normal(mean: number, sd: number): number;
  /** true with probability p. */
  chance(p: number): boolean;
  /** Uniform u32. */
  u32(): number;
}

export function makeRng(seed: number): Rng {
  let s = (seed >>> 0) || 0x9e3779b9;
  const u32 = (): number => {
    s ^= s << 13; s >>>= 0;
    s ^= s >>> 17;
    s ^= s << 5; s >>>= 0;
    return s;
  };
  const next = (): number => u32() / 0x100000000;
  return {
    next,
    u32,
    range: (lo, hi) => lo + next() * (hi - lo),
    int: (lo, hi) => lo + Math.floor(next() * (hi - lo + 1)),
    normal: (mean, sd) => {
      const u = Math.max(next(), 1e-12);
      const v = next();
      return mean + sd * Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * v);
    },
    chance: (p) => next() < p,
  };
}

/** Derives an independent seed for a sub-stream (per lane, per purpose). */
export function subSeed(seed: number, stream: number): number {
  let h = (seed ^ Math.imul(stream + 1, 0x9e3779b1)) >>> 0;
  h = Math.imul(h ^ (h >>> 16), 0x85ebca6b) >>> 0;
  h = Math.imul(h ^ (h >>> 13), 0xc2b2ae35) >>> 0;
  return (h ^ (h >>> 16)) >>> 0 || 1;
}
