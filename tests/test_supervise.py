"""Check the supervisor's credential isolation, lock, and false-pass guards."""
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools import supervise


class SupervisorTests(unittest.TestCase):
    def setUp(self):
        cache = Path(os.environ.get("WORM_TEST_BUILD_DIR", "H:/CodexCache/worm-ai-supervisor-tests")).resolve()
        cache.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=cache)
        self.root = Path(self.temporary.name).resolve()
        self.assertTrue(self.root.is_relative_to(cache))

    def tearDown(self):
        self.temporary.cleanup()

    def test_private_header_and_unrelated_file_are_not_copied(self):
        (self.root / "include").mkdir()
        (self.root / "src").mkdir()
        (self.root / "include/worm_secrets.h").write_text('#define WIFI_PASS "fixture-only"\n')
        (self.root / "include/worm_secrets.example.h").write_text('#define WIFI_PASS "CHANGE_ME"\n')
        (self.root / "src/main.cpp").write_text("void loop() {}\n")
        (self.root / "unrelated.pdf").write_bytes(b"fixture")
        names = {p.relative_to(self.root).as_posix() for p in supervise.public_files(self.root)}
        self.assertEqual(names, {"include/worm_secrets.example.h", "src/main.cpp"})

    def test_lock_prevents_overlapping_validation(self):
        with supervise.locked(self.root / "lock") as first:
            self.assertTrue(first)
            with supervise.locked(self.root / "lock") as second:
                self.assertFalse(second)
        with supervise.locked(self.root / "lock") as third:
            self.assertTrue(third)

    def test_zero_exit_without_test_summary_is_not_a_pass(self):
        result = supervise.run_check("native", [sys.executable, "-c", "print('no tests executed')"],
                                     self.root, os.environ.copy(), self.root / "native.log")
        self.assertEqual(result["exit_code"], 0)
        self.assertEqual(result["status"], "failed")

    def test_failing_exit_cannot_be_overridden_by_pass_text(self):
        command = [sys.executable, "-c", "print('1/1 regression cases passed'); raise SystemExit(2)"]
        result = supervise.run_check("native", command, self.root, os.environ.copy(), self.root / "native.log")
        self.assertEqual(result["exit_code"], 2)
        self.assertEqual(result["status"], "failed")

    def test_timeout_stops_the_started_process_tree(self):
        program = ("import subprocess,sys,time; "
                   "p=subprocess.Popen([sys.executable,'-c','import time; time.sleep(30)']); "
                   "print(p.pid,flush=True); time.sleep(30)")
        log = self.root / "timeout.log"
        result = supervise.run_check("timeout-fixture", [sys.executable, "-c", program],
                                     self.root, os.environ.copy(), log, timeout=1)
        self.assertEqual(result["status"], "timeout")
        pid = int(log.read_text(encoding="utf-8").splitlines()[0])
        if os.name == "nt":
            import ctypes
            api = ctypes.WinDLL("kernel32", use_last_error=True)
            api.OpenProcess.restype = ctypes.c_void_p
            api.CloseHandle.argtypes = [ctypes.c_void_p]
            api.GetExitCodeProcess.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
            handle = api.OpenProcess(0x1000, False, pid)
            if handle:
                code = ctypes.c_uint32()
                self.assertTrue(api.GetExitCodeProcess(handle, ctypes.byref(code)))
                api.CloseHandle(handle)
                self.assertNotEqual(code.value, 259)  # STILL_ACTIVE
            self.assertEqual(result["cleanup_exit_code"], 0)


if __name__ == "__main__":
    unittest.main()
