#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
import sys
from collections import Counter
from pathlib import Path


TITLE_RE = re.compile(r"^#\s*学习笔记（Tasks 1-(\d+)）\s*$")
TASK_HEADING_RE = re.compile(r"^##\s*Task\s+(\d+)\b")


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed < 1:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return parsed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--file", required=True, help="Path to study.md")
    parser.add_argument(
        "--expected-tasks",
        required=True,
        type=positive_int,
        help="Current milestone upper bound for documented tasks",
    )
    args = parser.parse_args()

    study_path = Path(args.file)
    if not study_path.is_file():
        print(f"[study-timeline] ERROR: file not found: {study_path}", file=sys.stderr)
        return 1

    lines = study_path.read_text(encoding="utf-8").splitlines()
    failures: list[str] = []

    title_task_count: int | None = None
    for line in lines:
        match = TITLE_RE.match(line.strip())
        if match:
            title_task_count = int(match.group(1))
            break

    if title_task_count is None:
        failures.append(
            "main title must match '# 学习笔记（Tasks 1-N）' with the documented highest task"
        )

    task_numbers: list[int] = []
    for line in lines:
        match = TASK_HEADING_RE.match(line.strip())
        if match:
            task_numbers.append(int(match.group(1)))

    if not task_numbers:
        failures.append(
            "no Task headings found; expected at least one heading like '## Task 1 - ...'"
        )
    else:
        counts = Counter(task_numbers)
        duplicates = sorted(number for number, count in counts.items() if count > 1)
        if duplicates:
            duplicate_list = ", ".join(str(number) for number in duplicates)
            failures.append(f"duplicate Task headings found: {duplicate_list}")

        unique_numbers = sorted(counts)
        highest_task = unique_numbers[-1]
        expected_sequence = list(range(1, highest_task + 1))
        missing_numbers = [
            number for number in expected_sequence if number not in counts
        ]

        if unique_numbers[0] != 1 or missing_numbers:
            problems: list[str] = []
            if unique_numbers[0] != 1:
                problems.append(f"sequence starts at {unique_numbers[0]} instead of 1")
            if missing_numbers:
                missing_list = ", ".join(str(number) for number in missing_numbers)
                problems.append(f"missing Task headings: {missing_list}")
            failures.append(
                "Task headings must be continuous from 1 to the highest documented task; "
                + "; ".join(problems)
            )

        if title_task_count is not None and title_task_count != highest_task:
            failures.append(
                "main title range does not match the highest documented task: "
                f"title says Tasks 1-{title_task_count}, headings reach Task {highest_task}"
            )

        if highest_task > args.expected_tasks:
            failures.append(
                f"highest documented task {highest_task} exceeds --expected-tasks {args.expected_tasks}"
            )

    if failures:
        for failure in failures:
            print(f"[study-timeline] ERROR: {failure}", file=sys.stderr)
        return 1

    highest_task = max(task_numbers)
    print(
        "[study-timeline] OK: documented Tasks 1-"
        f"{highest_task} in {study_path.as_posix()} (upper bound: {args.expected_tasks})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
