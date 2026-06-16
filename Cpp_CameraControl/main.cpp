// =============================================================================
// main.cpp
// =============================================================================
//
// This is the ENTRY POINT of the application — the first code that runs.
//
// In C++, every program starts by calling main(). For a Qt GUI application,
// main() does three things:
//   1. Creates a QApplication object (sets up the Qt event loop machinery)
//   2. Creates and shows the main window
//   3. Calls app.exec() to start the event loop (this blocks until the window closes)
//
// C++ CONCEPT — #include:
//   #include copies the contents of another file into this one at compile time.
//   Headers like <QApplication> declare classes without defining them fully;
//   the linker later resolves the actual code from the Qt library files.
// =============================================================================

// QApplication manages the entire Qt application lifecycle:
//   - Initializes Qt's internals (fonts, styles, event handling, etc.)
//   - Owns the event loop (app.exec())
//   - Must exist before ANY Qt widget is created
#include <QApplication>
#include <QCoreApplication>

// Our main window class
#include "MainWindow.h"

// ---- Windows crash-dump generation ------------------------------------------
//
// When the app crashes with an access violation, null pointer dereference, or
// similar exception, Windows calls our writeCrashDump() handler BEFORE showing
// the "application stopped working" dialog.  The handler writes a .dmp file
// next to the .exe.  To analyse it:
//   1. Copy both the .dmp and the matching .pdb to your dev machine.
//   2. Open the .dmp in Visual Studio — it will reconstruct the exact call stack,
//      local variable values, and thread state at the moment of the crash.
//
// DbgHelp.lib is a standard Windows SDK library — no extra install needed.
// It is always available on any Windows machine (ships with the OS).
// =============================================================================
#ifdef _WIN32
#include <windows.h>
#include <DbgHelp.h>

// Writes a minidump file to the same folder as the running .exe.
// 'ex' carries the exception record and the CPU register state at the crash.
// Returns EXCEPTION_CONTINUE_SEARCH so Windows still shows the normal
// "application stopped working" dialog after writing the dump.
static LONG WINAPI writeCrashDump(EXCEPTION_POINTERS* ex)
{
    // Build a timestamp-stamped filename so repeated crashes don't overwrite each other.
    SYSTEMTIME st;
    GetLocalTime(&st);

    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);

    // Replace the .exe filename with the dump filename (same folder).
    char* lastSlash = strrchr(exePath, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';  // Truncate to folder path

    char dumpPath[MAX_PATH];
    snprintf(dumpPath, MAX_PATH,
        "%sCrashDump_%04d%02d%02d_%02d%02d%02d.dmp",
        exePath,
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);

    HANDLE hFile = CreateFileA(dumpPath, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId          = GetCurrentThreadId();
        mei.ExceptionPointers = ex;
        mei.ClientPointers    = FALSE;

        // MiniDumpNormal: includes stack and code for all threads.
        // Small file (~1-5 MB), sufficient to pinpoint the crash location.
        // Change to MiniDumpWithFullMemory for heap contents too (~100-300 MB).
        MiniDumpWriteDump(
            GetCurrentProcess(),
            GetCurrentProcessId(),
            hFile,
            MiniDumpNormal,
            &mei,
            nullptr,
            nullptr);
        CloseHandle(hFile);

        // Show a message box so the user knows where the dump was saved.
        // This fires before Windows' "stopped working" dialog.
        MessageBoxA(nullptr,
            dumpPath,
            "Crash dump saved — copy this file + the .pdb to your dev machine",
            MB_OK | MB_ICONERROR);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

// =============================================================================
// Debug-only: intercept MSVC Runtime Check (RTC1) stack-corruption errors
// =============================================================================
//
// In a Debug build, MSVC places guard bytes around every local variable.
// If those guard bytes are overwritten, _RTC_DefaultErrorFuncW() fires and
// shows a dialog.  By replacing that function we can write the error to a
// log file so you can see WHICH variable was corrupted and in which file/line.
//
// This only compiles in Debug builds because RTC checks are stripped in Release.
// =============================================================================
#ifdef _DEBUG
#include <rtcapi.h>
#include <cstdio>
#include <ctime>

static int __cdecl rtcErrorHandler(int errType,
                                   const wchar_t* file,
                                   int            line,
                                   const wchar_t* module,
                                   const wchar_t* format,
                                   ...)
{
    // Build the exe folder path for the log file (same as crash dump).
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    char* lastSlash = strrchr(exePath, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';

    char logPath[MAX_PATH];
    snprintf(logPath, MAX_PATH, "%sRTC_StackCorruption.log", exePath);

    FILE* f = nullptr;
    fopen_s(&f, logPath, "a");
    if (f)
    {
        time_t t = time(nullptr);
        char   ts[64];
        ctime_s(ts, sizeof(ts), &t);
        ts[strcspn(ts, "\n")] = '\0';  // Strip trailing newline from ctime_s

        // errType meanings:
        //   _RTC_CHKSTK (0) — stack pointer corruption
        //   _RTC_CVRT   (1) — data-loss conversion (e.g. int → char)
        //   _RTC_STK    (2) — uninitialized variable or stack frame corruption
        fprintf(f,
            "[%s] RTC error type %d\n"
            "  File:   %ls\n"
            "  Line:   %d\n"
            "  Module: %ls\n",
            ts, errType,
            file   ? file   : L"(unknown)",
            line,
            module ? module : L"(unknown)");
        fclose(f);

        // Also pop a message box so the user knows the log was written.
        char msg[512];
        snprintf(msg, sizeof(msg),
            "Runtime stack-corruption error.\n\n"
            "Type: %d  Line: %d\n"
            "File: %ls\n\n"
            "Details appended to:\n%s",
            errType, line, file ? file : L"(unknown)", logPath);
        MessageBoxA(nullptr, msg, "RTC Stack Corruption", MB_OK | MB_ICONERROR);
    }

    return 1;  // Non-zero = abort (same behaviour as the default handler)
}
#endif // _DEBUG
#endif // _WIN32


// =============================================================================
// main()
// =============================================================================
//
// Parameters:
//   argc — number of command-line arguments (argument count)
//   argv — array of command-line argument strings (argument vector)
//   These are standard in every C/C++ program and passed in from the OS.
//
// Returns:
//   An integer exit code. 0 = success, non-zero = error.
//   app.exec() returns 0 when the window is closed normally.
int main(int argc, char* argv[])
{
    // Install the crash-dump handler FIRST — before any Qt or Arena code runs —
    // so that even a crash during QApplication construction is captured.
#ifdef _WIN32
    SetUnhandledExceptionFilter(writeCrashDump);

    // In Debug builds, also replace the RTC stack-corruption handler so the
    // error details are written to a log file next to the .exe.
#ifdef _DEBUG
    _RTC_SetErrorFuncW(rtcErrorHandler);
#endif
#endif

    // Create the application object. This MUST be the first Qt call.
    // Qt uses argc/argv to support standard command-line flags like --style, --platform, etc.
    QApplication app(argc, argv);

    // Set application metadata. This appears in window title bars and system dialogs.
    QApplication::setApplicationName("Lucid Camera Acquisition Tool");
    QApplication::setApplicationVersion("1.0");
    QApplication::setOrganizationName("DCS");

    // Set the visual style. "Fusion" is a clean, platform-independent Qt style
    // that looks consistent across Windows/Mac/Linux.
    QApplication::setStyle("Fusion");

    // Create and show the main window.
    // 'MainWindow window;' creates the object on the STACK (automatic storage).
    // When main() returns, 'window' is automatically destroyed.
    // This is different from 'new MainWindow()' which creates on the HEAP.
    MainWindow window;
    window.show();  // Makes the window visible (windows start hidden by default)

    // app.exec() starts the Qt event loop.
    // The event loop:
    //   1. Waits for events (mouse clicks, keyboard input, timer fires, etc.)
    //   2. Dispatches each event to the appropriate widget/handler
    //   3. Returns when the last window is closed (or QApplication::quit() is called)
    //
    // This call BLOCKS until the user closes the window.
    // The return value of exec() is passed back to the OS as our exit code.
    return app.exec();
}
