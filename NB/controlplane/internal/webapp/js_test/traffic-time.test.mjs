import test from "node:test";
import assert from "node:assert/strict";
import {centeredRange,parseLocalRange,presetRange} from "../assets/traffic-time.js";

test("preset range ends at now and remains live",()=>{
  const now=new Date("2026-08-19T02:00:00Z"),range=presetRange("2h",now);
  assert.equal(range.from.toISOString(),"2026-08-19T00:00:00.000Z");
  assert.equal(range.to.toISOString(),now.toISOString());
  assert.equal(range.live,true);
});

test("custom range becomes historical and validates order",()=>{
  const range=parseLocalRange("2026-08-19T08:00","2026-08-19T09:00");
  assert.equal(range.live,false);
  assert.throws(()=>parseLocalRange("2026-08-19T09:00","2026-08-19T08:00"));
});

test("event range centers fifteen minutes on each side",()=>{
  const range=centeredRange("2026-08-19T01:00:00Z");
  assert.equal(range.from.toISOString(),"2026-08-19T00:45:00.000Z");
  assert.equal(range.to.toISOString(),"2026-08-19T01:15:00.000Z");
});
