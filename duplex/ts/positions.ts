// Positions on a 32-bit wire (as wire.hh's uint32_t cursors): wrap-safe arithmetic, no BigInt.
export const ahead = (a: number, b: number): number => (a - b) | 0; // a - b as a signed 32-bit distance
export const add = (a: number, n: number): number => (a + n) >>> 0;

// What the peer's step decides, per quantum: is [p, p+Q) committed, and is it still unlapped?
export function inputReady(w: number, p: number, q: number, ringFrames: number, maxBlock: number): "ready" | "notYet" | "lapped" {
  if (ahead(w, add(p, q)) < 0) return "notYet";
  if (ahead(p, add(w, maxBlock - ringFrames)) < 0) return "lapped";
  return "ready";
}
