"""Compile and run firmware regressions with a host C++ compiler, without hardware."""
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import uuid


def main():
    root = Path(__file__).resolve().parents[1]
    compiler = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        raise SystemExit("Install a host g++/clang++ compiler or set CXX to its executable path.")
    default_cache = Path("H:/CodexCache/worm-ai-tests") if os.name == "nt" and Path("H:/").exists() else Path(tempfile.gettempdir()) / "worm-ai-tests"
    build = Path(os.environ.get("WORM_TEST_BUILD_DIR", default_cache)).resolve() / str(uuid.uuid4())
    build.mkdir(parents=True)
    binary = build / ("regressions.exe" if os.name == "nt" else "regressions")
    sources = ["tests/native/regressions.cpp", "src/brain/Genome.cpp", "src/brain/Brain.cpp",
               "src/net/WormNet.cpp", "src/repl/Replicator.cpp", "src/worm/Worm.cpp"]
    command = [compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-pedantic",
               "-I" + str(root / "tests/native"), "-I" + str(root / "src"),
               *(str(root / source) for source in sources), "-o", str(binary)]
    env = os.environ.copy()
    env["PATH"] = str(Path(compiler).resolve().parent) + os.pathsep + env.get("PATH", "")
    env["TEMP"] = env["TMP"] = str(build)
    options = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {"start_new_session": True}
    child = subprocess.Popen(command, env=env, **options)
    try:
        code = child.wait(timeout=120)
    except subprocess.TimeoutExpired:
        if os.name == "nt":
            subprocess.run(["taskkill", "/PID", str(child.pid), "/T", "/F"], check=True, timeout=15,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           creationflags=subprocess.CREATE_NO_WINDOW)
        else:
            os.killpg(child.pid, signal.SIGKILL)
        child.wait(timeout=15)
        raise
    if code:
        raise subprocess.CalledProcessError(code, command)
    subprocess.run([str(binary), *sys.argv[1:]], check=True, env=env, timeout=20)


if __name__ == "__main__":
    main()
