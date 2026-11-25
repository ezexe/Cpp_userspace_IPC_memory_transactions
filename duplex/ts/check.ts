import { add, inputReady } from "./positions";
declare const console: { log: (...a: unknown[]) => void };
const sab = new SharedArrayBuffer(64);
let f32Atomics = "accepted";
try { Atomics.load(new Float32Array(sab) as unknown as Int32Array, 0); } catch (e) { f32Atomics = `${(e as Error).name}: ${(e as Error).message}`; }
const big = new BigUint64Array(sab);
Atomics.store(big, 0, 5n);
console.log(`Atomics on Float32Array: ${f32Atomics}`);
console.log(`Atomics.load on BigUint64Array returns: ${typeof Atomics.load(big, 0)}`);
// The same three answers either side of 2^32.
let bad = 0, n = 0;
for (const base of [0, 0xffffffff - 300, 0x7fffffff - 300]) {
  for (let d = -9000; d < 9000; d += 7) {
    const p = add(base, 0), w = add(base, d);
    const want = d < 128 ? "notYet" : d - 128 >= 0 && -d < 512 - 8192 ? "lapped" : "ready";
    const got = inputReady(w, p, 128, 8192, 512);
    n++; if (got !== want) bad++;
  }
}
console.log(`wrap-safe readiness, ${n} cases across 0, 2^31 and 2^32: ${bad} wrong`);
