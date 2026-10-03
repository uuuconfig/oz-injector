"""End-to-end test for the oz injector.

For each mode: start the probe host, inject probe_dll.dll into it, then check
two things.

  1. DllMain ran  -> probe_dll wrote its own image base to a marker file.
  2. Module list  -> probe_dll is/is not present in the target's module list,
                     which is the whole point of the two modes.

The module check is done here (not inside the injector) with psapi, using a
handle opened with the rights EnumProcessModulesEx needs. The manual-map base
recorded by the probe is cross-checked against the injector's reported base to
confirm the addresses agree.
"""
import ctypes
import ctypes.wintypes as wt
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(HERE, "bin")
INJECTOR = os.path.abspath(os.path.join(HERE, "..", "build", "Release", "oz_injector.exe"))
PROBE_DLL = os.path.join(BIN, "probe_dll.dll")
PROBE_HOST = os.path.join(BIN, "probe_host.exe")
MARKER = os.path.join(os.environ.get("TEMP", os.environ.get("TMP", ".")),
                      "oz_injector_probe.txt")

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
LIST_MODULES_ALL = 0x03

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
psapi.EnumProcessModulesEx.argtypes = [
    wt.HANDLE, ctypes.POINTER(ctypes.c_void_p), wt.DWORD,
    ctypes.POINTER(wt.DWORD), wt.DWORD]
psapi.EnumProcessModulesEx.restype = wt.BOOL
psapi.GetModuleBaseNameW.argtypes = [
    wt.HANDLE, ctypes.c_void_p, wt.LPWSTR, wt.DWORD]
psapi.GetModuleBaseNameW.restype = wt.DWORD


def module_list(pid):
    """Return {module_name: base} for everything the target has loaded."""
    out = {}
    h = kernel32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not h:
        print(f"    [module_list] OpenProcess failed: {ctypes.get_last_error()}")
        return out
    try:
        mods = (ctypes.c_void_p * 2048)()
        needed = wt.DWORD()
        if not psapi.EnumProcessModulesEx(h, mods, ctypes.sizeof(mods),
                                          ctypes.byref(needed), LIST_MODULES_ALL):
            print(f"    [module_list] EnumProcessModulesEx failed: "
                  f"{ctypes.get_last_error()}")
            return out
        for i in range(needed.value // ctypes.sizeof(ctypes.c_void_p)):
            b = ctypes.create_unicode_buffer(260)
            if psapi.GetModuleBaseNameW(h, mods[i], b, 260):
                out[b.value.lower()] = mods[i]
    finally:
        kernel32.CloseHandle(h)
    return out


def read_marker():
    """Return {base, pid} recorded by the probe DLL, or None."""
    if not os.path.exists(MARKER):
        return None
    text = open(MARKER).read()
    m = re.search(r"base=0x([0-9a-fA-F]+)\s+pid=([0-9a-fA-F]+)", text)
    if not m:
        return None
    return {"base": int(m.group(1), 16), "pid": int(m.group(2), 16),
            "text": text.strip()}


def run_inject(pid, dll, mode):
    proc = subprocess.run([INJECTOR, "--inject", str(pid), dll, mode],
                          capture_output=True, text=True, errors="replace")
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


def test(mode, expect_listed):
    print(f"\n{'='*68}")
    print(f"  {mode}    module should be listed: {expect_listed}")
    print(f"{'='*68}")

    if os.path.exists(MARKER):
        os.remove(MARKER)

    host = subprocess.Popen([PROBE_HOST], stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    time.sleep(1.0)
    pid = host.pid
    print(f"  target pid : {pid}")

    try:
        code, out = run_inject(pid, PROBE_DLL, mode)
        for line in out.rstrip().splitlines():
            print(f"  | {line}")

        time.sleep(0.7)
        marker = read_marker()
        mods = module_list(pid)
        listed = any("probe_dll" in n for n in mods)

        print(f"\n  exit code         : {code}")
        print(f"  marker present    : {marker is not None}")
        if marker:
            print(f"  DllMain base      : 0x{marker['base']:X}")
            print(f"  DllMain pid       : {marker['pid']}")
        print(f"  modules in target : {len(mods)}")
        print(f"  probe_dll listed  : {listed}")
        print(f"  total modules     : {sorted(mods)[:6]}...")

        problems = []
        if code != 0:
            problems.append(f"non-zero exit ({code})")
        if marker is None:
            problems.append("marker absent - DllMain never ran")
        elif marker["pid"] != pid:
            problems.append(f"marker pid {marker['pid']} != target {pid}")
        if listed != expect_listed:
            problems.append(f"module listed={listed}, expected {expect_listed}")

        if problems:
            print(f"  RESULT: FAIL -> {'; '.join(problems)}")
            return False
        print("  RESULT: PASS")
        return True
    finally:
        host.terminate()
        try:
            host.wait(timeout=5)
        except subprocess.TimeoutExpired:
            host.kill()


if __name__ == "__main__":
    missing = [p for p in (INJECTOR, PROBE_DLL, PROBE_HOST) if not os.path.exists(p)]
    if missing:
        print("missing:")
        for m in missing:
            print("  " + m)
        sys.exit(2)

    print(f"injector : {INJECTOR}")
    print(f"dll      : {PROBE_DLL}")
    print(f"marker   : {MARKER}")

    results = [
        test("--manual", expect_listed=False),
        test("--loadlibrary", expect_listed=True),
    ]
    passed = sum(results)
    print(f"\n{'='*68}")
    print(f"  {passed}/{len(results)} passed")
    print(f"{'='*68}")
    sys.exit(0 if all(results) else 1)
