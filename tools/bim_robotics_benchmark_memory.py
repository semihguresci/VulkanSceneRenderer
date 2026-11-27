"""Host identity and process high-water marks for the BIM robotics benchmark."""

import ctypes
import os
from pathlib import Path
import platform


def proc_kib(path, key):
    for line in path.read_text(encoding="ascii").splitlines():
        if line.startswith(key + ":"):
            return int(line.split()[1]) * 1024
    return None


class WindowsMemoryStatus(ctypes.Structure):
    _fields_ = [("length", ctypes.c_uint32), ("load", ctypes.c_uint32)] + [
        (name, ctypes.c_uint64) for name in
        ("totalPhysical", "availablePhysical", "totalPagefile", "availablePagefile",
         "totalVirtual", "availableVirtual", "availableExtendedVirtual")
    ]


class WindowsProcessMemory(ctypes.Structure):
    _fields_ = [("length", ctypes.c_uint32), ("pageFaultCount", ctypes.c_uint32)] + [
        (name, ctypes.c_size_t) for name in
        ("peakWorkingSet", "workingSet", "quotaPeakPagedPool", "quotaPagedPool",
         "quotaPeakNonPagedPool", "quotaNonPagedPool", "pagefileUsage", "peakPagefileUsage", "privateUsage")
    ]


def host_identity():
    cpu = platform.processor() or os.environ.get("PROCESSOR_IDENTIFIER") or platform.machine()
    ram = None
    if os.name == "nt":
        import winreg
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as key:
                cpu = winreg.QueryValueEx(key, "ProcessorNameString")[0].strip()
        except OSError:
            pass
        memory = WindowsMemoryStatus(length=ctypes.sizeof(WindowsMemoryStatus))
        if ctypes.WinDLL("kernel32").GlobalMemoryStatusEx(ctypes.byref(memory)):
            ram = memory.totalPhysical
    elif platform.system() == "Linux":
        try:
            info = Path("/proc/cpuinfo").read_text(encoding="utf-8")
            cpu = next((line.split(":", 1)[1].strip() for line in info.splitlines() if
                        line.split(":", 1)[0].strip() == "model name"), cpu)
            ram = proc_kib(Path("/proc/meminfo"), "MemTotal")
        except OSError:
            pass
    return {"platform": platform.platform(), "architecture": platform.machine(), "cpu": cpu,
            "logicalCpus": os.cpu_count(), "physicalMemoryBytes": ram}


class ProcessMemory:
    """Collect the largest kernel high-water values seen while a child runs."""

    def __init__(self, pid):
        self.peak_resident = None
        self.peak_commit = None
        self.handle = None
        self.proc_status = None
        self.source = None
        if os.name == "nt":
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            open_process = kernel.OpenProcess
            open_process.argtypes = (ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32)
            open_process.restype = ctypes.c_void_p
            self.handle = open_process(0x0400 | 0x0010, False, pid)  # Query information and read memory counters
            self.close_handle = kernel.CloseHandle
            self.close_handle.argtypes = (ctypes.c_void_p,)
            self.get_memory = kernel.K32GetProcessMemoryInfo
            self.get_memory.argtypes = (ctypes.c_void_p, ctypes.POINTER(WindowsProcessMemory), ctypes.c_uint32)
            self.source = "Windows process memory counters"
        elif platform.system() == "Linux":
            self.proc_status = Path(f"/proc/{pid}/status")
            self.source = "Linux /proc process VmHWM"

    def __enter__(self):
        return self

    def __exit__(self, *_):
        if self.handle:
            self.close_handle(self.handle)
            self.handle = None

    def sample(self):
        if self.handle:
            counters = WindowsProcessMemory(length=ctypes.sizeof(WindowsProcessMemory))
            if self.get_memory(self.handle, ctypes.byref(counters), counters.length):
                if counters.peakWorkingSet:
                    self.peak_resident = max(self.peak_resident or 0, counters.peakWorkingSet)
                if counters.peakPagefileUsage:
                    self.peak_commit = max(self.peak_commit or 0, counters.peakPagefileUsage)
        elif self.proc_status:
            try:
                peak = proc_kib(self.proc_status, "VmHWM")
                if peak:
                    self.peak_resident = max(self.peak_resident or 0, peak)
            except OSError:
                pass  # The process can exit between the kernel sample and wait.

    def metrics(self):
        return {"source": self.source, "peakResidentBytes": self.peak_resident,
                "peakCommitBytes": self.peak_commit}
