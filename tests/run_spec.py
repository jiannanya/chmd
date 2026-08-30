#!/usr/bin/env python3
import argparse
import json
import subprocess
import sys


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    parser = argparse.ArgumentParser()
    parser.add_argument("--program", required=True)
    parser.add_argument("--spec", required=True)
    parser.add_argument("--example", type=int)
    parser.add_argument("--compact", action="store_true")
    args = parser.parse_args()

    with open(args.spec, encoding="utf-8") as stream:
        examples = json.load(stream)
    if args.example is not None:
        examples = [item for item in examples if item["example"] == args.example]

    failures = []
    for item in examples:
        process = subprocess.run(
            [args.program, "--to", "html"],
            input=item["markdown"].encode("utf-8"),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        actual = process.stdout.decode("utf-8", errors="replace")
        if process.returncode != 0 or actual != item["html"]:
            failures.append((item, actual, process.stderr.decode("utf-8", errors="replace")))

    if args.compact:
        print("failed examples:", " ".join(str(item["example"]) for item, _, _ in failures))
    for item, actual, stderr in ([] if args.compact else failures[:100]):
        print(f"FAIL example {item['example']} ({item['section']})")
        print("markdown:", repr(item["markdown"]))
        print("expected:", repr(item["html"]))
        print("actual:  ", repr(actual))
        if stderr:
            print("stderr:  ", stderr.rstrip())
    print(f"CommonMark: {len(examples) - len(failures)}/{len(examples)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
