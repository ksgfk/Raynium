"""Sample this client's Windows GPU Process Memory counters, independently of its graphics API."""
import ctypes


class _Value(ctypes.Structure):
    _fields_ = [('status', ctypes.c_ulong), ('value', ctypes.c_longlong)]


class _Item(ctypes.Structure):
    _fields_ = [('name', ctypes.c_wchar_p), ('value', _Value)]


class GpuMemory:
    def __init__(self, pid):
        self.api = ctypes.WinDLL('pdh')
        handle = ctypes.c_void_p
        dword = ctypes.c_ulong
        self.api.PdhOpenQueryW.argtypes = [ctypes.c_wchar_p, ctypes.c_size_t, ctypes.POINTER(handle)]
        self.api.PdhAddEnglishCounterW.argtypes = [handle, ctypes.c_wchar_p, ctypes.c_size_t, ctypes.POINTER(handle)]
        self.api.PdhCollectQueryData.argtypes = [handle]
        self.api.PdhGetFormattedCounterArrayW.argtypes = [handle, dword, ctypes.POINTER(dword), ctypes.POINTER(dword), ctypes.c_void_p]
        self.api.PdhCloseQuery.argtypes = [handle]
        for name in ('PdhOpenQueryW', 'PdhAddEnglishCounterW', 'PdhCollectQueryData', 'PdhGetFormattedCounterArrayW', 'PdhCloseQuery'):
            getattr(self.api, name).restype = dword
        self.query = handle()
        self.counters = {}
        self.samples = 0
        status = self.api.PdhOpenQueryW(None, 0, ctypes.byref(self.query))
        if status: raise OSError(status, 'Cannot open GPU memory query')
        try:
            for name, counter in [('sampledGpuDedicatedBytes', 'Dedicated Usage'), ('sampledGpuSharedBytes', 'Shared Usage'),
                                  ('sampledGpuCommittedBytes', 'Total Committed')]:
                value = handle()
                status = self.api.PdhAddEnglishCounterW(self.query, f'\\GPU Process Memory(pid_{pid}_*)\\{counter}', 0, ctypes.byref(value))
                if status: raise OSError(status, 'Cannot add GPU memory counter: '+counter)
                self.counters[name] = value
        except BaseException:
            self.close()
            raise

    def sample(self):
        if self.api.PdhCollectQueryData(self.query): return {}
        result = {}
        for name, counter in self.counters.items():
            # The array contains one entry per adapter; a process can acquire adapters after startup.
            for _ in range(3):
                size, count = ctypes.c_ulong(), ctypes.c_ulong()
                status = self.api.PdhGetFormattedCounterArrayW(counter, 0x400, ctypes.byref(size), ctypes.byref(count), None)
                if status != 0x800007D2 or not size.value: break  # PDH_MORE_DATA
                buffer = ctypes.create_string_buffer(size.value)
                status = self.api.PdhGetFormattedCounterArrayW(counter, 0x400, ctypes.byref(size), ctypes.byref(count), buffer)
                if status == 0x800007D2: continue
                if status: break
                values = [item.value.value for item in (_Item*count.value).from_buffer(buffer) if item.value.status in (0, 1)]
                if values: result[name] = sum(values)
                break
        if result: self.samples += 1
        return result

    def close(self):
        if self.query:
            self.api.PdhCloseQuery(self.query)
            self.query = None
