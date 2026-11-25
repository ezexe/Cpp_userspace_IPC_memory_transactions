// The ShadowCell layout from utx.hpp, read and written through Atomics: word 0 is ver, then two slots.
export function shadowWrite(cell: BigUint64Array, value: BigUint64Array): void {
  const cur = Atomics.load(cell, 0);
  const base = 1 + Number((cur + 1n) & 1n) * value.length;
  for (let i = 0; i < value.length; i++) Atomics.store(cell, base + i, value[i]!);
  Atomics.store(cell, 0, cur + 1n); // the commit point
}

export function shadowTryRead(cell: BigUint64Array, out: BigUint64Array): boolean {
  const v1 = Atomics.load(cell, 0);
  const base = 1 + Number(v1 & 1n) * out.length;
  for (let i = 0; i < out.length; i++) out[i] = Atomics.load(cell, base + i);
  return Atomics.load(cell, 0) === v1;
}
