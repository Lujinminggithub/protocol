import test from "node:test";
import assert from "node:assert/strict";
import {healthTone,initialPositions} from "../assets/topology-layout.js";

test("initial topology places entry left of relay and exit",()=>{
  const devices=[{id:"exit"},{id:"entry"},{id:"relay"}],links=[{source:"entry",target:"relay"},{source:"relay",target:"exit"}];
  const positions=initialPositions(devices,links);
  assert.ok(positions.entry.x<positions.relay.x);
  assert.ok(positions.relay.x<positions.exit.x);
});

test("saved database position wins over derived layout",()=>{
  const positions=initialPositions([{id:"entry",layout:{x:77,y:8,z:9}},{id:"exit"}],[{source:"entry",target:"exit"}]);
  assert.deepEqual(positions.entry,{x:77,y:8,z:9});
});

test("cyclic inventory still produces a bounded layout",()=>{
  const positions=initialPositions([{id:"a"},{id:"b"}],[{source:"a",target:"b"},{source:"b",target:"a"}]);
  assert.deepEqual(Object.keys(positions).sort(),["a","b"]);
  assert.ok(Number.isFinite(positions.a.x)&&Number.isFinite(positions.b.x));
});

test("health tone maps warning and critical states",()=>{
  assert.equal(healthTone("healthy"),"normal");
  assert.equal(healthTone("degraded"),"warning");
  assert.equal(healthTone("offline"),"critical");
});
