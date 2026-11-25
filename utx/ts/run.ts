import { shadowTryRead, shadowWrite } from "./shadow";
declare const require: (m: string) => any;
declare const __filename: string;
const wt = require("node:worker_threads");
const kWords = 512;
if (wt.isMainThread) {
  const sab = new SharedArrayBuffer(8 * (1 + 2 * kWords));
  const cell = new BigUint64Array(sab);
  const worker = new wt.Worker(__filename, { workerData: sab });
  const out = new BigUint64Array(kWords);
  let ok = 0, retry = 0, torn = 0;
  const until = Date.now() + 1500;
  while (Date.now() < until) {
    if (!shadowTryRead(cell, out)) { retry++; continue; }
    ok++;
    for (let i = 1; i < kWords; i++) if (out[i] !== out[0]) { torn++; break; }
  }
  console.log(`ts ShadowCell: ok=${ok} retry=${retry} torn=${torn} last=${out[0]}`);
  worker.terminate();
} else {
  const cell = new BigUint64Array(wt.workerData as SharedArrayBuffer);
  const v = new BigUint64Array(kWords);
  for (let i = 1n; ; i++) { v.fill(i); shadowWrite(cell, v); }
}
