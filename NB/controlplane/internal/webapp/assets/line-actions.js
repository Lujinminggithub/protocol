const normallyDeletable = new Set(["draft", "disabled", "archived"]);

export function lineDeletionAction(status, hasSpec, hasActiveOperation, forceEligible) {
  if (status === "deleting") return {mode:"blocked", label:"清理中", reason:"正在清理节点实例和端口"};
  if (hasActiveOperation) return {mode:"blocked", label:"任务执行中", reason:"线路仍有排队中或执行中的任务"};
  if (!hasSpec || normallyDeletable.has(status)) return {mode:"normal", label:"删除"};
  return forceEligible ? {mode:"force", label:"强制删除"} : {mode:"normal", label:"删除"};
}
