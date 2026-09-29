import test from "node:test";
import assert from "node:assert/strict";
import {lineDeletionAction} from "../assets/line-actions.js";

test("line deletion action distinguishes normal, force, and blocked deletion", () => {
  assert.deepEqual(lineDeletionAction("disabled", true, false, false), {mode:"normal", label:"删除"});
  assert.deepEqual(lineDeletionAction("active", true, false, true), {mode:"force", label:"强制删除"});
  assert.deepEqual(lineDeletionAction("maintenance", true, false, true), {mode:"force", label:"强制删除"});
  assert.deepEqual(lineDeletionAction("active", true, true, true), {mode:"blocked", label:"任务执行中", reason:"线路仍有排队中或执行中的任务"});
  assert.deepEqual(lineDeletionAction("active", false, false, false), {mode:"normal", label:"删除"});
  assert.deepEqual(lineDeletionAction("deleting", true, false, false), {mode:"blocked", label:"清理中", reason:"正在清理节点实例和端口"});
  assert.deepEqual(lineDeletionAction("active", true, false, false), {mode:"normal", label:"删除"});
});
