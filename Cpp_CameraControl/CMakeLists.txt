# =============================================================================
# CMakeLists.txt - Build configuration for GenICamControl
# =============================================================================
#
# CMake is a "build system generator" — it doesn't build your code directly.
# Instead it generates the project files (e.g., a Visual Studio .sln) that
# your compiler/IDE then uses to actually compile things.
#
# To use this with Visual Studio 2022:
#   1. Open Visual Studio 2022
#   2. File → Open → CMake...  (or "Open a local folder" and point to this dir)
#   3. VS will automatically detect CMakeLists.txt and configure the project
#   4. Select "GenICamControl.exe" as the startup item and build
#
# Alternatively, from the command line:
#   cmake -B build -G "Visual Studio 17 2022" -A x64
#   cmake --build build --config Release
# =============================================================================

# Require at minimum CMake 3.22. This version has full Qt 6 CMake support.
cmake_minimum_required(VERSION 3.22)

# Name the project and declare that we use C++
project(GenICamControl LANGUAGES CXX)

# Use C++17. This is required for features like structured bindings, std::optional,
# and other modern C++ conveniences used throughout this project.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)   # Error if the compiler doesn't support C++17
set(CMAKE_CXX_EXTENSIONS OFF)         # Use standard C++17, not compiler-specific extensions

# =============================================================================
# Qt 6 Setup
# =============================================================================
#
# CMAKE_PREFIX_PATH tells CMake where to search for packages.
# We point it at the Qt 6.11.1 MSVC 2022 64-bit installation.
# If you reinstall Qt to a different location, update this path.
set(QT_ROOT "C:/ProgramData/Qt/6.11.1/msvc2022_64")
list(APPEND CMAKE_PREFIX_PATH "${QT_ROOT}")

# find_package locates Qt6 and its component libraries.
# REQUIRED means CMake will error out if Qt6 is not found.
# We need:
#   - Core    → QThread, QMutex, signals/slots, QString, etc.
#   - Gui     → QFont, QIcon, QColor, etc.
#   - Widgets → QMainWindow, QPushButton, QComboBox, etc.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

# qt_standard_project_setup() is a Qt 6 helper that automatically enables:
#   - AUTOMOC  → runs the Meta-Object Compiler (moc) on headers with Q_OBJECT
#   - AUTORCC  → compiles .qrc resource files
#   - AUTOUIC  → compiles .ui designer files into C++ headers
# Without AUTOMOC, signals and slots won't work!
qt_standard_project_setup()

# =============================================================================
# Arena SDK Setup
# =============================================================================
#
# The Arena SDK is Lucid Vision Labs' camera control library.
# It implements the GenICam standard for camera configuration and streaming.
# LUCID_DEV_ROOT is set by the Arena SDK installer; we hard-code it here.
set(ARENA_SDK_ROOT "C:/Program Files/Lucid Vision Labs/Arena SDK")

# =============================================================================
# Source files
# =============================================================================
#
# We list every .cpp file that needs to be compiled.
# Headers (.h) are listed too so they appear in the IDE's file tree,
# but they don't need to be compiled separately.
set(SOURCES
    main.cpp
    MainWindow.cpp
    CameraManager.cpp
    AcquisitionWorker.cpp
    AdvancedParamsDialog.cpp
    PinnedParamsPanel.cpp
    PreviewWorker.cpp
    PreviewDialog.cpp
    FocusDiagnosticDialog.cpp
    ZoomableImageWidget.cpp   # Zoom/pan image display used in PreviewDialog
    HistogramWidget.cpp       # Rotated histogram with level handles used in PreviewDialog
    ConfigDialog.cpp          # Preferences dialog for auto-contrast percentile thresholds
    AutoStopDialog.cpp        # Preferences dialog for timed auto-stop acquisition
    TiffWriter.cpp            # Hand-rolled multi-page TIFF writer (no external library)
    app.rc              # Windows resource file — embeds app.ico into the .exe
)

set(HEADERS
    MainWindow.h
    CameraManager.h
    AcquisitionWorker.h
    AdvancedParamsDialog.h
    PinnedParamsPanel.h
    PreviewWorker.h
    PreviewDialog.h
    FocusDiagnosticDialog.h
    ZoomableImageWidget.h
    HistogramWidget.h
    ConfigDialog.h            # Preferences dialog declaration + static settings accessors
    AutoStopDialog.h          # Timed auto-stop dialog declaration + static settings accessors
    TiffWriter.h              # TiffWriter class declaration
)

# =============================================================================
# Create the executable
# =============================================================================
#
# qt_add_executable is the Qt 6 replacement for plain add_executable.
# WIN32 tells the linker to create a Windows GUI app (no console window).
# If you want a console window for debug output, remove WIN32.
qt_add_executable(GenICamControl WIN32
    ${SOURCES}
    ${HEADERS}
    resources.qrc   # Qt resource file — embeds assets/video-camera.png into the .exe
)

# Release builds go directly to the shared deployment folder.
# Debug builds stay in the default CMake build tree.
set_target_properties(GenICamControl PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}"
)

# =============================================================================
# Include directories
# =============================================================================
#
# These are the folders the compiler searches when you write #include "SomeHeader.h"
# PRIVATE means they are only used when compiling this target, not exposed
# to other CMake targets that might depend on this one.
target_include_directories(GenICamControl PRIVATE
    # Main Arena SDK camera API: ISystem, IDevice, IImage, etc.
    "${ARENA_SDK_ROOT}/include/Arena"

    # GenTL (Generic Transport Layer): low-level interface for transport protocols
    "${ARENA_SDK_ROOT}/include/GenTL"

    # Save API: helpers for saving images to PNG/BMP/TIFF/etc (not used here,
    # but included so we can #include SaveApi.h if needed later)
    "${ARENA_SDK_ROOT}/include/Save"

    # GenICam CPP library: the GenApi node system (INode, INodeMap, etc.)
    # This is the standard that all GigE Vision cameras use for parameters.
    "${ARENA_SDK_ROOT}/GenICam/library/CPP/include"
)

# =============================================================================
# Library search directories
# =============================================================================
#
# These are the folders the linker searches for .lib files.
target_link_directories(GenICamControl PRIVATE
    # All Arena SDK import libraries live here (both Debug and Release variants)
    "${ARENA_SDK_ROOT}/lib64/Arena"

    # GenICam support libraries (GCBase, GenApi, log4cpp, etc.)
    # The Arena SDK headers use #pragma comment(lib, "...") to auto-link these.
    # The linker needs to know where to search for them.
    # Win64_x64 contains both debug (MDd) and release (MD) variants.
    "${ARENA_SDK_ROOT}/GenICam/library/CPP/lib/Win64_x64"
)

# =============================================================================
# Preprocessor definitions
# =============================================================================
#
# These are like #define but set for the entire project without touching any file.
target_compile_definitions(GenICamControl PRIVATE
    # GenICam requires this define to compile with MSVC.
    # Without it, you get "unsupported compiler" errors from GenICam headers.
    GENICAM_USER_ACCEPTS_ANY_COMPILER

    # Enable Unicode strings throughout the project.
    # On Windows, UNICODE makes Win32 functions use wide (UTF-16) strings by default.
    UNICODE
    _UNICODE

    # Exclude rarely-used headers from Windows.h (speeds up compilation)
    WIN32_LEAN_AND_MEAN

    # Prevent Windows.h from defining min() and max() as macros.
    # Without this, #include <windows.h> (pulled in by Qt or Arena SDK) defines
    # min(a,b) and max(a,b) as macros that silently replace std::min(a,b) calls,
    # causing compilation errors whenever the argument types can't be deduced.
    # Explicit template forms like std::min<int64_t>() sidestep the macro but
    # NOMINMAX is the correct global fix.
    NOMINMAX

    # Suppress MSVC's "deprecated function" warnings for older CRT functions.
    # Only needed if you use things like sprintf(); prefer snprintf() in new code.
    _CRT_SECURE_NO_WARNINGS
)

# =============================================================================
# Link libraries
# =============================================================================
#
# $<$<CONFIG:Debug>:Arenad_v140> is a CMake "generator expression".
# It reads: "if the build configuration is Debug, use Arenad_v140 (the debug lib)"
# The debug Arena lib has extra checks and symbols; release is faster.
target_link_libraries(GenICamControl PRIVATE
    Qt6::Core       # Qt core (threading, strings, events)
    Qt6::Gui        # Qt GUI foundation
    Qt6::Widgets    # Qt widget toolkit (buttons, windows, etc.)

    # Arena SDK — debug and release have different .lib names
    $<$<CONFIG:Debug>:Arenad_v140>
    $<$<CONFIG:Release>:Arena_v140>

    # Arena Save API — for saving images as TIFF files
    # Debug and release have different .lib names
    $<$<CONFIG:Debug>:Saved_v140>
    $<$<CONFIG:Release>:Save_v140>

    # DbgHelp — Windows SDK library for writing crash minidumps.
    # Ships with Windows; no extra install needed.
    DbgHelp

    # iphlpapi — Windows IP Helper API (GetAdaptersInfo, ForceIP NIC enumeration).
    # Used by CameraManager::tryForceIpConnect() to find a compatible NIC subnet
    # when the camera is on the wrong IP after a reboot.
    iphlpapi
)

# =============================================================================
# OpenMP — multi-core parallelization for the Welford field-capture pixel loop
# =============================================================================
#
# find_package(OpenMP) locates MSVC's built-in OpenMP 2.0 support.
# When found, linking OpenMP::OpenMP_CXX adds /openmp to the compiler flags,
# which activates all #pragma omp directives in the source.
# Without this, every #pragma omp line is silently ignored and the pixel loop
# runs on a single core.
#
# MSVC ships with OpenMP support out of the box — no extra install needed.
# The if() guard makes it non-fatal on compilers that don't have it.
find_package(OpenMP)
if(OpenMP_CXX_FOUND)
    target_link_libraries(GenICamControl PRIVATE OpenMP::OpenMP_CXX)
endif()

# =============================================================================
# Post-build: Deploy Qt DLLs
# =============================================================================
#
# After the .exe is built, run Qt's windeployqt tool.
# windeployqt scans the .exe and copies all required Qt DLLs and plugins
# (like platforms/qwindows.dll) next to the executable.
# Without this step, the .exe will fail to start with "DLL not found" errors.
add_custom_command(TARGET GenICamControl POST_BUILD
    COMMAND "${QT_ROOT}/bin/windeployqt.exe"
            --no-translations
            --no-system-d3d-compiler
            "$<TARGET_FILE:GenICamControl>"
    COMMENT "Deploying Qt DLLs with windeployqt..."
)


# =============================================================================
# Post-build: Copy assets folder to build output directory
# =============================================================================
#
# The assets folder contains the software manual (manual.html) and the camera
# hardware documentation (ATX245S_1.21.0.0_Documentation_English/html/).
# Both are opened by the Help menu at runtime using QDesktopServices::openUrl.
# copy_directory mirrors the entire folder tree next to the executable each build.
add_custom_command(TARGET GenICamControl POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:GenICamControl>/assets"
    COMMENT "Copying assets (manuals, icons) to build output directory..."
)

# =============================================================================
# Note on Arena SDK DLLs
# =============================================================================
#
# The Arena SDK installer adds its x64Release and x64Debug folders to the
# system PATH, so the runtime loader finds those DLLs automatically.
# If you move the .exe to a machine without the Arena SDK, you'll need to copy:
#   Release: C:/Program Files/Lucid Vision Labs/Arena SDK/x64Release/Arena_v140.dll
#   Debug:   C:/Program Files/Lucid Vision Labs/Arena SDK/x64Debug/Arenad_v140.dll
# (and their dependencies) next to the .exe.
