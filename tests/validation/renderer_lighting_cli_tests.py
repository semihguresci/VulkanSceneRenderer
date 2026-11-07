"""Exercise lighting option parsing without initializing the renderer or GPU."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess


STOP = "--renderer-lighting-cli-test-stop"


def check(exe: Path):
    cases = []

    def invoke(arguments, expected):
        command = [str(exe), *arguments, STOP]
        result = subprocess.run(command, cwd=exe.parent, capture_output=True, text=True,
                                errors="replace", timeout=20,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        text = result.stdout + result.stderr
        assert result.returncode != 0, (arguments, text)
        if expected is None:
            # Reaching the final unknown option proves every prior option parsed.
            # Both success and failure paths exit before Application is constructed.
            assert "unknown argument: " + STOP in text, (arguments, text)
        else:
            assert expected in text and STOP not in text, (arguments, text)
        cases.append(dict(arguments=arguments, valid=expected is None,
                          returnCode=result.returncode, diagnostic=text.strip()))

    ranges = {"--environment-intensity": ("0", "16", "-0.001", "16.0000001"),
              "--directional-intensity": ("0", "16", "-0.001", "16.0000001"),
              "--bounce-intensity": ("0", "2", "-0.001", "2.0000001"),
              "--environment-yaw": ("-180", "180", "-180.000001", "180.000001")}
    for option, (low, high, under, over) in ranges.items():
        for value in (low, high):
            invoke([option, value], None)
        for value in (under, over, "nan", "inf", "-inf", "0.5junk", "", "1e5000"):
            invoke([option, value], option)
    invoke(["--environment-intensity", "+0.5"], None)
    invoke(["--directional-intensity", "1e1"], None)
    invoke(["--bounce-intensity", "+0.5"], None)
    invoke(["--bounce-intensity", "2e0"], None)
    invoke(["--environment-intensity", "16", "--directional-intensity", "0",
            "--bounce-intensity", "0", "--environment-yaw", "-180"], None)
    return dict(passed=True, gpuInitialized=False, caseCount=len(cases), cases=cases)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = check(args.exe.resolve(strict=True))
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("x", encoding="utf-8") as stream:
            json.dump(result, stream, indent=2)
            stream.write("\n")
    print("PASS", result["caseCount"], "lighting CLI parse cases; GPU not initialized")


if __name__ == "__main__":
    main()
