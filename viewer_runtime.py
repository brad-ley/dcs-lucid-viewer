"""Windows DLL setup required before importing Qt or other native packages."""
import sys

_windows_icu = None


def prepare_qt_runtime():
    """Load Windows' ICU before conda's incompatible same-named DLL.

    The pip Qt runtime uses Windows ICU's unversioned exports. Conda's ICU
    exports different symbols from its own icuuc.dll. Loading the System32
    library first keeps Qt's dependency resolution consistent in activated
    conda environments. Retain the handle for the lifetime of the process.
    """
    global _windows_icu
    if sys.platform == "win32" and _windows_icu is None:
        import ctypes
        # LOAD_LIBRARY_SEARCH_SYSTEM32 also restricts dependency lookup.
        _windows_icu = ctypes.WinDLL("icuuc.dll", winmode=0x00000800)

