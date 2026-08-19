const normallyDeletable = new Set(["draft", "disabled", "archived"]);

export function lineDeletionAction(status, hasSpec, hasActiveOperation, forceEligible) {
  if (hasActiveOperation) return {mode:"blocked", label:"任务执行中", reason:"线路仍有排队中或执行中的任务"};
  if (!hasSpec || normallyDeletable.has(status)) return {mode:"normal", label:"删除"};
  return forceEligible ? {mode:"force", label:"强制删除"} : {mode:"blocked", label:"请先停用", reason:"线路节点正常，请先执行停用"};
}
