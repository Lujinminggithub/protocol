"""
数学计算器工具 - 基于 ast 白名单的安全表达式求值，不使用 eval()。

只允许数字常量、四则运算/乘方、括号、一元正负号，以及少量白名单数学函数/常量。
任何不在白名单内的语法节点(变量引用之外的Name、任意函数调用、属性访问、
下标、比较、布尔运算、lambda、导入等)一律拒绝，从根源杜绝任意代码执行。
"""

import ast
import json
import math
import operator

_BINOPS = {
    ast.Add: operator.add,
    ast.Sub: operator.sub,
    ast.Mult: operator.mul,
    ast.Div: operator.truediv,
    ast.FloorDiv: operator.floordiv,
    ast.Mod: operator.mod,
    ast.Pow: operator.pow,
}
_UNARYOPS = {
    ast.UAdd: operator.pos,
    ast.USub: operator.neg,
}
_FUNCS = {
    "sqrt": math.sqrt,
    "pow": math.pow,
    "abs": abs,
    "round": round,
    "sin": math.sin,
    "cos": math.cos,
    "tan": math.tan,
    "log": math.log,
    "log10": math.log10,
    "exp": math.exp,
    "floor": math.floor,
    "ceil": math.ceil,
}
_NAMES = {
    "pi": math.pi,
    "e": math.e,
}


class SafeEvalError(Exception):
    pass


def _eval(node):
    if isinstance(node, ast.Expression):
        return _eval(node.body)
    if isinstance(node, ast.Constant) and isinstance(node.value, (int, float)) and not isinstance(node.value, bool):
        return node.value
    if isinstance(node, ast.BinOp) and type(node.op) in _BINOPS:
        return _BINOPS[type(node.op)](_eval(node.left), _eval(node.right))
    if isinstance(node, ast.UnaryOp) and type(node.op) in _UNARYOPS:
        return _UNARYOPS[type(node.op)](_eval(node.operand))
    if (
        isinstance(node, ast.Call)
        and isinstance(node.func, ast.Name)
        and node.func.id in _FUNCS
        and not node.keywords
    ):
        return _FUNCS[node.func.id](*[_eval(a) for a in node.args])
    if isinstance(node, ast.Name) and node.id in _NAMES:
        return _NAMES[node.id]
    raise SafeEvalError(f"不支持的表达式节点: {type(node).__name__}")


def run(args: dict) -> str:
    expr = str(args.get("expression", ""))
    if not expr.strip():
        return json.dumps({"error": "expression 不能为空"}, ensure_ascii=False)
    try:
        tree = ast.parse(expr, mode="eval")
        value = _eval(tree)
        return json.dumps({"expression": expr, "result": value}, ensure_ascii=False)
    except (SafeEvalError, SyntaxError, ZeroDivisionError, TypeError, ValueError, OverflowError) as e:
        return json.dumps({"error": f"表达式无法计算: {e}"}, ensure_ascii=False)
