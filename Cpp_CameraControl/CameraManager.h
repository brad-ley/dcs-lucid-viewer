// =============================================================================
// CameraManager.h
// =============================================================================
//
// This header declares the CameraManager class and the data structures it uses.
//
// WHAT THIS FILE DOES:
//   CameraManager is a "wrapper" around the Lucid Arena SDK. Its job is to hide
//   the complexity of the Arena SDK behind a simpler interface that the rest of
//   the application can use without knowing all the SDK details.
//
// C++ CONCEPT — Header vs Source files:
//   In C++, code is split into two file types:
//     .h  (header)  → declares WHAT exists (classes, functions, variables)
//     .cpp (source) → defines HOW it works (the actual code)
//   Other files #include the header to know what's available.
//   The #pragma once at the top prevents the header from being included twice.
// =============================================================================

#pragma once   // "Include this file only once per compilation unit" — prevents duplicate definitions

// --- Standard C++ Library Headers ---
// These come with every C++ compiler and provide common utilities.
#include <string>    // std::string — a text string class
#include <vector>    // std::vector — a resizable array
#include <cstdint>   // int64_t, uint8_t, etc. — fixed-size integer types

// Qt
#include <QString>   // returned by getNodeEnumString()

// --- Arena SDK Headers ---
// ArenaApi.h is the main "umbrella" header for the Arena SDK.
// Including it gives us access to Arena::ISystem, Arena::IDevice,
// Arena::IImage, and all other SDK classes/interfaces.
#include "ArenaApi.h"


// =============================================================================
// CameraInfo — info about one discovered camera on the network
// =============================================================================
//
// C++ CONCEPT — struct:
//   A struct is a bundle of related data grouped under one name.
//   Unlike a class, struct members are public by default.
//   We use this to pass camera info from CameraManager to the UI.
struct CameraInfo
{
    std::string displayName;   // Human-readable label, e.g. "TRI028S-CC (SN: 2412345)"
    std::string modelName;     // Camera model, e.g. "TRI028S-CC"
    std::string serialNumber;  // Unique serial number, e.g. "2412345"
    std::string ipAddress;     // Camera's IP address on the network, e.g. "192.168.1.10"
    std::string macAddress;    // Camera's hardware MAC address, e.g. "1c:0f:af:12:34:56"
};


// =============================================================================
// CameraParameters — a snapshot of the camera's key settings
// =============================================================================
//
// This struct holds the current values AND the allowed ranges for each setting.
// Ranges are needed so the UI can configure spinbox min/max values correctly.
//
// GenICam nodes (the camera's parameter system) are typed:
//   - Float nodes  → map to C++ double
//   - Integer nodes → map to C++ int64_t
//   - Enumeration nodes → map to a string (symbolic name like "Continuous")
struct CameraParameters
{
    // --- Exposure ---
    // ExposureAuto: can be "Off", "Once", or "Continuous"
    std::string              exposureAuto;
    std::vector<std::string> exposureAutoOptions;  // All valid options for the dropdown

    // ExposureTime: in microseconds (μs). Only writable when ExposureAuto == "Off"
    double exposureTime;
    double exposureTimeMin;  // Minimum allowed value (from camera node)
    double exposureTimeMax;  // Maximum allowed value (from camera node)

    // --- Gain ---
    // GainAuto: can be "Off", "Once", or "Continuous"
    std::string              gainAuto;
    std::vector<std::string> gainAutoOptions;

    // Gain: in decibels (dB). Only writable when GainAuto == "Off"
    double gain;
    double gainMin;
    double gainMax;

    // --- Image dimensions ---
    // Width and Height in pixels. Must be multiples of their 'Inc' (increment) value.
    int64_t width;
    int64_t widthMin;
    int64_t widthMax;
    int64_t widthInc;   // Step size: width must be a multiple of this (e.g., 8)

    int64_t height;
    int64_t heightMin;
    int64_t heightMax;
    int64_t heightInc;

    // --- Frame rate ---
    // AcquisitionFrameRate: frames per second. Some cameras don't expose this node.
    double frameRate;
    double frameRateMin;
    double frameRateMax;
    bool   frameRateAvailable;  // false if this camera doesn't have the node

    // --- Pixel format ---
    // The encoding of each pixel, e.g. "Mono8", "BayerRG8", "RGB8Packed"
    std::string              pixelFormat;
    std::vector<std::string> pixelFormatOptions;
};


// =============================================================================
// CameraManager class
// =============================================================================
//
// C++ CONCEPT — class:
//   A class bundles data (member variables) and behaviour (methods/functions).
//   'public:' members are accessible from outside the class.
//   'private:' members are only accessible from within the class.
//
// This class manages the entire lifecycle of working with an Arena SDK camera:
//   System init → discover cameras → connect → read/write params → disconnect → shutdown
class CameraManager
{
public:
    // ----- Constructor & Destructor -----

    // Constructor: called automatically when you create a CameraManager object.
    // Sets member pointers to nullptr (C++ doesn't zero-initialize pointers automatically).
    CameraManager();

    // Destructor: called automatically when the object goes out of scope or is deleted.
    // We use it to make sure the Arena system is shut down even if the user forgets.
    // The ~ prefix means "destructor".
    ~CameraManager();

    // ----- System Lifecycle -----

    // Initialize the Arena SDK system. Must be the very first call.
    // Returns true on success. On failure, errorMsg will describe what went wrong.
    //
    // C++ CONCEPT — pass by reference (&):
    //   std::string& errorMsg means we pass a reference to an existing string.
    //   The function can write to it, and the caller sees the result.
    //   This is more efficient than returning a new string.
    bool initializeSystem(std::string& errorMsg);

    // Clean up and release all Arena SDK resources.
    // Safe to call even if nothing was initialized.
    void shutdownSystem();

    // ----- Device Discovery -----

    // Scan the network for cameras (sends broadcast discovery packets).
    // Returns a list of CameraInfo structs, one per discovered camera.
    // Pass a timeout in milliseconds — longer timeout finds more cameras on slow networks.
    std::vector<CameraInfo> discoverCameras(std::string& errorMsg, int timeoutMs = 1000);

    // ----- Device Connection -----

    // Connect to the camera with the given serial number.
    // You must call discoverCameras() at least once first.
    bool connectCamera(const std::string& serialNumber, std::string& errorMsg);

    // Rebuild a CameraInfo for the given serial from the current m_deviceInfoList.
    // Returns true and fills 'info' if the serial is found; false otherwise.
    // Use this after connectCamera() to get the real IP (which may differ from
    // what discoverCameras() reported if ForceIP reassigned the address).
    bool getUpdatedCameraInfo(const std::string& serialNumber, CameraInfo& info) const;

    // Disconnect the current camera and free its resources.
    // Safe to call even if no camera is connected.
    void disconnectCamera();

    // Stop the camera stream without disconnecting.
    // Used by PreviewDialog::stopPreviewIfRunning() when the preview worker doesn't
    // exit within its first timeout — StopStream() breaks any blocking GetImage() call
    // so the worker thread can finish cleanly before the device is destroyed.
    // Safe to call if no stream is running (the SDK is a no-op).
    void stopStream();

    // Returns true if a camera is currently connected and ready to use.
    bool isConnected() const;

    // ----- Parameter Reading -----

    // Read all current parameter values from the connected camera.
    // Populates 'params' with live values AND their allowed ranges.
    bool readParameters(CameraParameters& params, std::string& errorMsg);

    // ----- Parameter Writing -----

    // Write a complete set of parameters to the connected camera.
    // The order of writes matters: e.g., set ExposureAuto before ExposureTime.
    bool applyParameters(const CameraParameters& params, std::string& errorMsg);

    // ----- Direct SDK Access -----

    // Returns the raw IDevice pointer so the AcquisitionWorker can use it directly.
    // IMPORTANT: Do not call this while acquisition is running — the acquisition
    // thread is using this device and concurrent access could cause crashes.
    //
    // C++ CONCEPT — const method:
    //   The 'const' at the end means this method doesn't modify the object.
    //   It promises "I will only read, not write, any member variables."
    Arena::IDevice* getDevice() const { return m_pDevice; }

    // Returns the GenICam node map for the connected camera, or nullptr if not connected.
    // The node map is the "dictionary" of all camera parameters — you can look up any
    // named parameter (node) from it and read or write its value.
    // Used by: MainWindow (dynamic param widgets), AdvancedParamsDialog.
    GenApi::INodeMap* getNodeMap() const
    {
        return m_pDevice ? m_pDevice->GetNodeMap() : nullptr;
    }

    // ----- Node reading helpers (also used by AdvancedParamsDialog and MainWindow) -----

    // Read all valid symbolic names for an enumeration node (e.g., ExposureAuto options).
    // Returns an empty vector if the node doesn't exist or isn't readable.
    std::vector<std::string> getEnumOptions(const std::string& nodeName);

    // Read the current string value of an enumeration node.
    // Returns an empty string if the node doesn't exist or isn't readable.
    std::string getEnumValue(const std::string& nodeName);

    // Read the current value of a float (double) node.
    // Returns 'fallback' if the node doesn't exist or isn't readable.
    double getDoubleValue(const std::string& nodeName, double fallback = 0.0);

    // Read the current value of an integer (int64_t) node.
    // Returns 'fallback' if the node doesn't exist or isn't readable.
    int64_t getInt64Value(const std::string& nodeName, int64_t fallback = 0);

    // Snap rawValue to the nearest value the camera's integer node will accept.
    // GenICam integer nodes require: (value - min) % inc == 0.
    // This returns: min + round((rawValue - min) / inc) * inc, clamped to [min, max].
    // Falls back to rawValue unchanged if the node is unavailable or unreadable.
    int64_t snapInt64Value(const std::string& nodeName, int64_t rawValue);

    // Like snapInt64Value but overrides the live node max with a caller-supplied value.
    // Needed for OffsetX/OffsetY when drawing an ROI: the live node max equals
    // (SensorWidth - CurrentWidth), which is wrong when the target width differs
    // from the current width.  Pass (SensorDim - targetDim) as overrideMax.
    int64_t snapInt64ValueWithMax(const std::string& nodeName,
                                  int64_t rawValue,
                                  int64_t overrideMax);

    // Check if a node exists and is readable on the current device.
    bool isNodeAvailable(const std::string& nodeName);

    // ----- Node writing helpers -----
    //
    // These write a single named parameter to the camera.
    // They are used by:
    //   - MainWindow::onApplyParametersClicked()  (dynamic param widgets)
    //   - AdvancedParamsDialog::onApplyClicked()  (any parameter browser)
    //
    // All return true on success. On failure they write a description into errorMsg.

    // Read the current symbolic value of an enumeration node (e.g., "AcquisitionMode" → "Continuous").
    // Returns an empty QString if the node does not exist, is not readable, or no camera is connected.
    QString getNodeEnumString(const std::string& nodeName) const;

    // Write a symbolic enum value (e.g., set "ExposureAuto" → "Off").
    bool setNodeEnumValue(const std::string& nodeName, const std::string& value,
                          std::string& errorMsg);

    // Write a floating-point value (e.g., set "ExposureTime" → 5000.0).
    // The value is automatically clamped to the node's [min, max] range.
    bool setNodeDoubleValue(const std::string& nodeName, double value,
                            std::string& errorMsg);

    // Write an integer value (e.g., set "Width" → 1920).
    // The value is automatically clamped to the node's [min, max] range.
    bool setNodeInt64Value(const std::string& nodeName, int64_t value,
                           std::string& errorMsg);

    // Write a boolean value (e.g., set "AcquisitionFrameRateEnable" → true).
    bool setNodeBoolValue(const std::string& nodeName, bool value,
                          std::string& errorMsg);

private:
    // Attempts to ForceIP the camera identified by 'mac' to a compatible IP on a
    // local NIC subnet, then rediscovers and connects.  Sets m_pDevice on success.
    // Called by connectCamera() when CreateDevice() fails with an unknown error.
    bool tryForceIpConnect(uint64_t mac, std::string& statusMsg);

    // ----- Private member variables -----
    //
    // C++ CONVENTION — m_ prefix:
    //   Member variables often use an "m_" prefix to distinguish them from
    //   local variables and function parameters. This is a style convention,
    //   not a C++ requirement.
    //
    // C++ CONCEPT — raw pointers (*):
    //   ISystem* means "a pointer to an ISystem object."
    //   Pointers store a memory address rather than the value directly.
    //   The Arena SDK creates these objects for us and gives us pointers.
    //   We are responsible for telling the SDK when we're done with them.

    Arena::ISystem* m_pSystem;   // The Arena SDK "system" — manages all camera connections
    Arena::IDevice* m_pDevice;   // The currently connected camera (nullptr if none)

    // Cached list of discovered cameras, used when connecting by serial number
    std::vector<Arena::DeviceInfo> m_deviceInfoList;
};
