#!/usr/bin/env python3
"""Classify Jeandle safepoint-related calls in textual LLVM IR.

Final statepoint totals are not a safepoint-elimination metric: lowering
turns polls into direct handler calls, RewriteStatepointsForGC wraps many
unrelated calls into statepoints, and late unrolling clones retained polls.
Tests and triage should assert on the four categories separately.
"""
import argparse
import sys

CATEGORIES = (
    "explicit_polls",            # call @jeandle.safepoint_poll
    "direct_handler_calls",      # lowered poll: direct call to the handler
    "poll_derived_statepoints",  # gc.statepoint targeting the poll/handler
    "nonpoll_statepoints",       # any other gc.statepoint
)


def classify(lines, handler):
    counts = dict.fromkeys(CATEGORIES, 0)
    for line in lines:
        s = line.strip()
        if s.startswith(("declare", "define", ";")):
            continue
        if "call" not in s and "invoke" not in s:
            continue
        if "@llvm.experimental.gc.statepoint" in s:
            if "@jeandle.safepoint_poll" in s or handler in s:
                counts["poll_derived_statepoints"] += 1
            else:
                counts["nonpoll_statepoints"] += 1
        elif "@jeandle.safepoint_poll" in s:
            counts["explicit_polls"] += 1
        elif handler in s:
            counts["direct_handler_calls"] += 1
    return counts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="*", help="IR files (default: stdin)")
    parser.add_argument("--handler-name", default="safepoint_handler",
                        help="symbol the poll lowers its slow path to")
    args = parser.parse_args()

    lines = []
    if args.files:
        for name in args.files:
            with open(name) as f:
                lines += f.readlines()
    else:
        lines = sys.stdin.readlines()

    for category, count in classify(lines, args.handler_name).items():
        print(f"{category}\t{count}")


if __name__ == "__main__":
    main()
