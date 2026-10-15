"""Validate a public source snapshot; skip unchanged results and prevent overlapping runs.

This script does not edit source or invoke Git mutations. Codex reviews new failures
and performs the separately authorized, bounded repair workflow.
"""
import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import shutil
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
DIRECTORIES = ("src", "include", "tests", "tools", "sim", "docs", "skills")
SUFFIXES = {".cpp", ".h", ".py", ".md", ".ini", ".txt", ".json", ".yml", ".yaml"}
TOP_FILES = ("platformio.ini", "requirements-dev.txt", "README.md", "README.zh-CN.md",
             "AGENTS.md", ".gitignore", ".gitattributes", ".editorconfig")


def public_files(root):
    paths = subprocess.run(["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                           cwd=root, capture_output=True, check=False)
    if paths.returncode == 0:
        candidates = [root / name.decode("utf-8") for name in paths.stdout.split(b"\0") if name]
    else:
        candidates = [root / name for name in TOP_FILES]
        for directory in DIRECTORIES:
            candidates.extend((root / directory).rglob("*"))
    selected = []
    for path in candidates:
        relative = path.relative_to(root)
        if relative.as_posix() == "include/worm_secrets.h" or not path.is_file():
            continue
        if len(relative.parts) == 1:
            allowed = relative.name in TOP_FILES
        else:
            allowed = relative.parts[0] in DIRECTORIES and path.suffix in SUFFIXES
        if allowed:
            if not path.resolve().is_relative_to(root.resolve()):
                raise ValueError("Public source resolves outside the project: " + str(relative))
            selected.append(path)
    return sorted(set(selected))


def fingerprint(root, files, compiler, core):
    digest = hashlib.sha256()
    for path in files:
        digest.update(path.relative_to(root).as_posix().encode("utf-8") + b"\0")
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    digest.update(sys.version.encode("utf-8"))
    resolved = shutil.which(compiler) or compiler
    binary = Path(resolved).resolve()
    if binary.is_file():
        stat = binary.stat()
        digest.update(f"{binary}:{stat.st_size}:{stat.st_mtime_ns}".encode("utf-8"))
    else:
        digest.update(str(binary).encode("utf-8"))
    for relative in ("platforms/espressif32/platform.json", "packages/framework-arduinoespressif32/package.json",
                     "packages/toolchain-xtensa-esp32s3/package.json"):
        metadata = core / relative
        digest.update(metadata.read_bytes() if metadata.is_file() else ("missing:" + relative).encode("utf-8"))
    try:
        from importlib.metadata import version
        digest.update(version("platformio").encode("utf-8"))
    except Exception:
        digest.update(b"platformio-unavailable")
    return digest.hexdigest()


@contextmanager
def locked(path):
    handle = path.open("a+b")
    if handle.tell() == 0:
        handle.write(b"0")
        handle.flush()
    handle.seek(0)
    acquired = False
    try:
        if os.name == "nt":
            import msvcrt
            msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        acquired = True
    except OSError:
        pass
    try:
        yield acquired
    finally:
        if acquired:
            if os.name == "nt":
                handle.seek(0)
                msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(handle.fileno(), fcntl.LOCK_UN)
        handle.close()


def save(path, data):
    pending = path.with_suffix(".pending.json")
    pending.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    pending.replace(path)


def run_check(name, command, project, env, log, timeout=240):
    started = time.monotonic()
    record = {"name": name, "status": "failed", "log": str(log)}
    with log.open("w", encoding="utf-8") as output:
        try:
            options = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {"start_new_session": True}
            child = subprocess.Popen(command, cwd=project, env=env, stdout=output, stderr=subprocess.STDOUT, **options)
            try:
                record["exit_code"] = child.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                # Stop only this command's process tree, including SCons/compiler children.
                if os.name == "nt":
                    cleanup = subprocess.run(["taskkill", "/PID", str(child.pid), "/T", "/F"],
                                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15, check=False,
                                             creationflags=subprocess.CREATE_NO_WINDOW)
                    record["cleanup_exit_code"] = cleanup.returncode
                else:
                    os.killpg(child.pid, signal.SIGKILL)
                child.wait(timeout=15)
                raise
            if record["exit_code"] == 0:
                record["status"] = "passed"
        except subprocess.TimeoutExpired:
            record["status"] = "timeout"
        except OSError as error:
            record["error"] = str(error)
    record["seconds"] = round(time.monotonic() - started, 3)
    text = log.read_text(encoding="utf-8", errors="replace")
    if record["status"] == "passed" and name == "native":
        match = re.search(r"(\d+)/(\d+) regression cases passed", text)
        if not match or int(match[1]) == 0 or match[1] != match[2]:
            record["status"] = "failed"
        record["benchmarks"] = [json.loads(line[10:]) for line in text.splitlines() if line.startswith("BENCHMARK ")]
    if record["status"] == "passed" and name == "firmware" and "[SUCCESS]" not in text:
        record["status"] = "failed"
    if record["status"] == "passed" and name == "firmware":
        record["memory"] = re.findall(r"(?:RAM|Flash):.*used \d+ bytes from \d+ bytes", text)
    return record


def validate(args, cache):
    compiler = args.cxx or os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++") or "g++"
    compiler = shutil.which(compiler) or compiler
    core = Path(os.environ.get("PLATFORMIO_CORE_DIR", "H:/CodexCache/platformio")).resolve()
    if os.name == "nt" and core.drive.upper() != "H:":
        raise RuntimeError("PLATFORMIO_CORE_DIR must use drive H")
    files = public_files(ROOT)
    current = fingerprint(ROOT, files, compiler, core)
    state_file = cache / "status.json"
    if state_file.exists() and not args.force:
        previous = json.loads(state_file.read_text(encoding="utf-8"))
        if previous.get("fingerprint") == current:
            outcome = "unchanged" if previous.get("status") == "passed" else "unchanged_failure"
            print(json.dumps({"status": outcome, "evidence": str(state_file),
                              "previous_status": previous.get("status")}, ensure_ascii=False))
            return 0 if outcome == "unchanged" else 1
    if shutil.disk_usage(cache).free < 1024**3:
        raise RuntimeError("At least 1 GiB of free cache space is required")
    run = cache / "runs" / str(uuid.uuid4())
    project = run / "project"
    for source in files:
        target = project / source.relative_to(ROOT)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    temporary = run / "tmp"
    temporary.mkdir()
    env = os.environ.copy()
    env.update(CXX=compiler, PYTHONDONTWRITEBYTECODE="1", PYTHONUTF8="1", TEMP=str(temporary), TMP=str(temporary),
               PLATFORMIO_CORE_DIR=str(core), PIP_CACHE_DIR=str(cache.parent / "pip"),
               PLATFORMIO_BUILD_CACHE_DIR=str(cache.parent / "platformio-build-cache"),
               WORM_TEST_BUILD_DIR=str(run / "native-build"), PLATFORMIO_SETTING_ENABLE_TELEMETRY="false",
               # PlatformIO interprets 0 as checking on every run, not disabling checks.
               # Validation uses pinned tools; avoid unrelated network upgrade checks.
               PLATFORMIO_SETTING_CHECK_PLATFORMIO_INTERVAL="36500")
    commands = [("native", [sys.executable, "tests/run_native.py"]),
                ("supervisor", [sys.executable, "tests/test_supervise.py"]),
                ("simulator", [sys.executable, "sim/worm_sim.py"]),
                ("firmware", [sys.executable, "-m", "platformio", "run", "-e", "esp32-s3-devkitc-1", "--jobs", "4"])]
    record = {"fingerprint": current, "status": "passed", "source": str(ROOT), "snapshot": str(project),
              "at_utc": datetime.now(timezone.utc).isoformat(), "checks": []}
    for name, command in commands:
        check_env = env.copy()
        if name == "firmware":
            check_env.pop("CXX", None)  # PlatformIO selects its pinned Xtensa compiler.
        check = run_check(name, command, project, check_env, run / (name + ".log"))
        record["checks"].append(check)
        if check["status"] != "passed":
            record["status"] = "failed"
            break
    if fingerprint(ROOT, public_files(ROOT), compiler, core) != current:
        record["status"] = "source_changed_during_validation"
    save(run / "result.json", record)
    save(state_file, record)
    print(json.dumps({"status": record["status"], "evidence": str(state_file),
                      "checks": record["checks"]}, ensure_ascii=False))
    return 0 if record["status"] == "passed" else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache-dir", type=Path, default=Path("H:/CodexCache/worm-ai-supervision"))
    parser.add_argument("--cxx", help="Path to a host C++ compiler, or use CXX")
    parser.add_argument("--force", action="store_true", help="Revalidate even if source and tool identity are unchanged")
    args = parser.parse_args()
    cache = args.cache_dir.resolve()
    if os.name == "nt" and (not Path("H:/").is_dir() or cache.drive.upper() != "H:"):
        raise SystemExit("Drive H is required for Codex caches; no fallback was used")
    cache.mkdir(parents=True, exist_ok=True)
    with locked(cache / "supervision.lock") as acquired:
        if not acquired:
            print(json.dumps({"status": "busy", "message": "Another validation is running"}))
            return 0
        return validate(args, cache)


if __name__ == "__main__":
    raise SystemExit(main())
