#!/usr/bin/env python3
"""Make patcher-rs/assets/xp-missing.txt: the Windows functions an engine may
import that Windows XP doesn't have, for the check after building the legacy
XP engine (engines::check_xp_imports).

The source are ReactOS's .spec files (ReactOS is a reimplementation of Windows
Server 2003, and marks every function of later Windows versions with
-version=0x600+ and the like). ReactOS targets Server 2003 and doesn't mark
the functions that came with it, so those (from YY-Thunks, which records the
first Windows version of each function) are listed by hand below: XP lacks
them (GetThreadId stopped the first XP engine). Only DLLs the engine uses are
listed. XP's C
runtime (msvcrt.dll 7.0) has none of the "secure" *_s functions; those are
caught by the check itself.

    tools/xp-missing.py > patcher-rs/assets/xp-missing.txt
"""
import re
import sys
import urllib.request

BASE = "https://raw.githubusercontent.com/reactos/reactos/master/"
SPECS = {
    "kernel32.dll": "dll/win32/kernel32/kernel32.spec",
    "user32.dll": "win32ss/user/user32/user32.spec",
    "gdi32.dll": "win32ss/gdi/gdi32/gdi32.spec",
    "advapi32.dll": "dll/win32/advapi32/advapi32.spec",
    "shell32.dll": "dll/win32/shell32/shell32.spec",
    "ole32.dll": "dll/win32/ole32/ole32.spec",
    "oleaut32.dll": "dll/win32/oleaut32/oleaut32.spec",
    "ws2_32.dll": "dll/win32/ws2_32/ws2_32.spec",
    "iphlpapi.dll": "dll/win32/iphlpapi/iphlpapi.spec",
    "winmm.dll": "dll/win32/winmm/winmm.spec",
    "version.dll": "dll/win32/version/version.spec",
    "setupapi.dll": "dll/win32/setupapi/setupapi.spec",
    "imm32.dll": "win32ss/user/imm32/imm32.spec",
    "crypt32.dll": "dll/win32/crypt32/crypt32.spec",
    "imagehlp.dll": "dll/win32/imagehlp/imagehlp.spec",
    "dbghelp.dll": "dll/win32/dbghelp/dbghelp.spec",
    "shlwapi.dll": "dll/win32/shlwapi/shlwapi.spec",
}
XP = 0x501
# Server 2003 (NT 5.2) and its SP1, not in XP SP3 (YY-Thunks' __WindowsNT5_2*)
SERVER_2003 = {
    "kernel32.dll": ["ConvertThreadToFiberEx", "DecodePointer", "EncodePointer", "FlsAlloc", "FlsFree",
                     "FlsGetValue", "FlsSetValue", "GetCurrentProcessorNumber", "GetLargePageMinimum",
                     "GetProcessIdOfThread", "GetProcessWorkingSetSizeEx", "GetSystemFirmwareTable",
                     "GetThreadId", "GetProcessId", "InterlockedCompareExchange64", "IsNLSDefinedString",
                     "ReOpenFile", "SetProcessWorkingSetSizeEx", "SetThreadStackGuarantee",
                     "Wow64DisableWow64FsRedirection", "Wow64EnableWow64FsRedirection",
                     "Wow64RevertWow64FsRedirection"],
    "advapi32.dll": ["CreateProcessWithTokenW", "RegDeleteKeyExA", "RegDeleteKeyExW", "RegGetValueA",
                     "RegGetValueW"],
    "psapi.dll": ["QueryWorkingSetEx"],
    "user32.dll": ["IsWow64Message"],
}


def on_xp(tag):
    if tag is None:
        return True
    m = re.match(r"0x([0-9A-Fa-f]+)(\+|-0x([0-9A-Fa-f]+))?$", tag)
    lo = int(m.group(1), 16)
    if m.group(2) == "+":
        return lo <= XP
    if m.group(3):
        return lo <= XP <= int(m.group(3), 16)
    return lo == XP


def main():
    print("# Windows functions Windows XP doesn't have (DLL function), from ReactOS's")
    print("# export lists; made by tools/xp-missing.py")
    for dll, path in SPECS.items():
        with urllib.request.urlopen(BASE + path, timeout=60) as r:
            text = r.read().decode("utf-8", "replace")
        avail = {}
        for line in text.splitlines():
            line = line.split("#")[0].strip()
            if not line.startswith("@"):
                continue
            tag = re.search(r"-version=(\S+)", line)
            toks = [t for t in line.split()[2:] if not t.startswith("-")]
            if not toks:
                continue
            name = re.split(r"[(\s]", toks[0])[0]
            avail[name] = avail.get(name, False) or on_xp(tag.group(1) if tag else None)
        for name in SERVER_2003.get(dll, []):
            avail[name] = False
        for name in sorted(n for n, ok in avail.items() if not ok):
            print(dll, name)
    for name in SERVER_2003.get("psapi.dll", []):
        print("psapi.dll", name)


if __name__ == "__main__":
    sys.exit(main())
