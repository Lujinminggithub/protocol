#!/usr/bin/env python3
from soak_test import apply_collection_tolerance


def case(reasons):
    return {"passed": not reasons, "failure_reasons": list(reasons),
            "load": {"integrity": "count-ok"},
            "integrity_before": {"integrity": "ok"},
            "integrity_after": {"integrity": "ok"}}


first = case(["collection-error"])
assert apply_collection_tolerance(first, 0, 2) == 1
assert first["passed"] and first["collection_error_tolerated"]
second = case(["collection-error"])
assert apply_collection_tolerance(second, 1, 2) == 2
assert not second["passed"]
data_failure = case(["collection-error", "throughput"])
assert apply_collection_tolerance(data_failure, 0, 2) == 0
assert not data_failure["passed"]
clean = case([])
assert apply_collection_tolerance(clean, 1, 2) == 0
print("RESULT PASS")
