// =============================================================================
// CameraManager.cpp
// =============================================================================
//
// This file contains the implementation of the CameraManager class declared
// in CameraManager.h. Every method declared there is defined here.
//
// KEY CONCEPTS IN THIS FILE:
//   - Arena SDK system/device lifecycle
//   - GenICam node map — the standard way to read/write camera parameters
//   - Exception handling with try/catch
//   - GenICam typed node pointers (CFloatPtr, CIntegerPtr, CEnumerationPtr, etc.)
// =============================================================================

// iphlpapi must be included before our own header because ArenaApi.h pulls in
// windows.h, and GetAdaptersInfo / IP_ADAPTER_INFO live in iphlpapi.h.
// iphlpapi.h depends on windows.h being included first — it is, via ArenaApi.h —
// but the header itself has no winsock conflict so no special ordering is needed.
#include <windows.h>
#include <iphlpapi.h>

// Include our own header first — this is good practice so we catch any
// missing #includes that the header itself needs.
#include "CameraManager.h"

// Standard C++ headers
#include <algorithm>  // std::find_if — searching through containers
#include <stdexcept>  // std::runtime_error (not used directly, but good to have)
#include <vector>     // std::vector — for adapter info buffer

// GenApi headers — the GenICam parameter/node system
// GenApi is the part of GenICam that handles reading and writing camera settings.
// It defines concepts like "node maps", "float nodes", "enum nodes", etc.
#include "GenApi/GenApi.h"


// =============================================================================
// Constructor
// =============================================================================
// When CameraManager is created, initialize all pointers to nullptr.
// In C++, member pointers are NOT automatically set to null — you must do it
// yourself or you'll get "garbage" values that cause crashes.
CameraManager::CameraManager()
    : m_pSystem(nullptr)   // ": name(value)" is the "member initializer list"
    , m_pDevice(nullptr)   // This is more efficient than assigning in the body
{
    // Body is empty — all initialization is done above in the initializer list
}


// =============================================================================
// Destructor
// =============================================================================
// Called automatically when the object is destroyed (e.g., goes out of scope,
// or the parent window closes). We ensure cleanup happens no matter what.
CameraManager::~CameraManager()
{
    // Always disconnect and shut down cleanly on destruction.
    // These functions are safe to call even if nothing was connected.
    disconnectCamera();
    shutdownSystem();
}


// =============================================================================
// initializeSystem
// =============================================================================
bool CameraManager::initializeSystem(std::string& errorMsg)
{
    // Guard: don't initialize twice
    if (m_pSystem != nullptr)
    {
        return true;  // Already initialized
    }

    // C++ CONCEPT — try/catch:
    // "try" runs code that might throw an exception (an error object).
    // "catch" handles the exception if one is thrown.
    // GenICam/Arena SDK functions throw exceptions on errors rather than
    // returning error codes like C APIs do.
    try
    {
        // Arena::OpenSystem() is the entry point to the Arena SDK.
        // It initializes the underlying network transport layers and
        // returns a pointer to the singleton ISystem object.
        // Think of ISystem as the "camera factory" — it discovers and creates devices.
        m_pSystem = Arena::OpenSystem();

        return true;  // Success
    }
    // GenICam::GenericException is the base class for all Arena SDK exceptions.
    // "const&" means we catch a const reference to the exception — efficient and safe.
    catch (const GenICam::GenericException& e)
    {
        // e.what() returns a C-style string (const char*) describing the error.
        // We assign it to errorMsg so the caller can display it.
        errorMsg = std::string("Arena SDK error: ") + e.what();
        return false;
    }
    catch (const std::exception& e)
    {
        // Catch any other standard C++ exceptions as a fallback
        errorMsg = std::string("Unexpected error: ") + e.what();
        return false;
    }
}


// =============================================================================
// shutdownSystem
// =============================================================================
void CameraManager::shutdownSystem()
{
    // Make sure any connected device is disconnected first.
    // Trying to close the system while a device is open would be a bug.
    disconnectCamera();

    if (m_pSystem != nullptr)
    {
        try
        {
            // Arena::CloseSystem() releases all SDK resources and shuts down
            // the transport layer. Must be called to cleanly exit.
            Arena::CloseSystem(m_pSystem);
        }
        catch (...) { /* Ignore errors during shutdown */ }

        m_pSystem = nullptr;  // Set to null so we don't accidentally use it again
    }
}


// =============================================================================
// discoverCameras
// =============================================================================
std::vector<CameraInfo> CameraManager::discoverCameras(std::string& errorMsg, int timeoutMs)
{
    std::vector<CameraInfo> result;  // We'll fill this and return it

    if (m_pSystem == nullptr)
    {
        errorMsg = "System not initialized. Call initializeSystem() first.";
        return result;  // Return empty list
    }

    try
    {
        // UpdateDevices() broadcasts discovery packets on all network interfaces
        // and waits up to 'timeoutMs' milliseconds for cameras to respond.
        // More time = more reliable discovery on slow networks.
        m_pSystem->UpdateDevices(timeoutMs);

        // GetDevices() returns a vector of DeviceInfo objects, one per found camera.
        // DeviceInfo contains the camera's model, serial, IP, MAC, etc.
        m_deviceInfoList = m_pSystem->GetDevices();

        // Convert each Arena::DeviceInfo into our simpler CameraInfo struct
        // DeviceInfo methods (ModelName, SerialNumber, etc.) are not const-qualified,
        // so we must iterate by non-const reference.
        for (Arena::DeviceInfo& devInfo : m_deviceInfoList)
        {
            CameraInfo info;

            // gcstring has c_str() returning const char*, so we construct std::string from it.
            // gcstring does NOT have operator std::string() — use c_str() instead.
            info.modelName    = std::string(devInfo.ModelName().c_str());
            info.serialNumber = std::string(devInfo.SerialNumber().c_str());
            info.ipAddress    = std::string(devInfo.IpAddressStr().c_str());
            info.macAddress   = std::string(devInfo.MacAddressStr().c_str());

            // Build a human-readable label for the UI dropdown
            info.displayName = info.modelName + "  (SN: " + info.serialNumber + ")"
                             + "  [" + info.ipAddress + "]";

            result.push_back(info);  // Add to the result list
        }
    }
    catch (const GenICam::GenericException& e)
    {
        errorMsg = std::string("Discovery error: ") + e.what();
    }
    catch (const std::exception& e)
    {
        errorMsg = std::string("Discovery error: ") + e.what();
    }

    return result;
}


// =============================================================================
// connectCamera
// =============================================================================
bool CameraManager::connectCamera(const std::string& serialNumber, std::string& errorMsg)
{
    if (m_pSystem == nullptr)
    {
        errorMsg = "System not initialized.";
        return false;
    }

    // Disconnect any previously connected camera first
    disconnectCamera();

    // Search the cached device list for the camera with the matching serial number.
    // This is done outside the try block so the MAC address remains accessible in
    // the catch handler (local variables declared inside try are out of scope there).
    //
    // C++ CONCEPT — lambda and std::find_if:
    //   std::find_if searches a range and returns an iterator to the first element
    //   that satisfies a predicate (a function that returns true/false).
    //   The [&] lambda captures all local variables by reference.
    auto it = std::find_if(
        m_deviceInfoList.begin(),
        m_deviceInfoList.end(),
        [&serialNumber](Arena::DeviceInfo& devInfo)  // non-const: DeviceInfo methods aren't const-qualified
        {
            return std::string(devInfo.SerialNumber().c_str()) == serialNumber;
        }
    );

    if (it == m_deviceInfoList.end())
    {
        errorMsg = "Camera with serial number '" + serialNumber + "' not found. "
                   "Try refreshing the camera list.";
        return false;
    }

    // Save MAC now so the catch handler can pass it to tryForceIpConnect even
    // though the iterator is not accessible inside a catch block.
    const uint64_t deviceMac = it->MacAddress();

    try
    {
        // CreateDevice() opens a connection to the physical camera.
        // Returns an IDevice* that we use for all camera operations.
        m_pDevice = m_pSystem->CreateDevice(*it);  // *it dereferences the iterator

        if (m_pDevice == nullptr)
        {
            errorMsg = "CreateDevice() returned null unexpectedly.";
            return false;
        }

        return true;
    }
    catch (const GenICam::GenericException& e)
    {
        // Translate common SDK error codes into plain-English messages so the user
        // knows what to do without reading the raw Arena exception stack.
        std::string raw = e.what();

        if (raw.find("GC_ERR_ACCESS_DENIED") != std::string::npos
            || raw.find("ACCESS_DENIED")      != std::string::npos
            || raw.find("access denied")      != std::string::npos)
        {
            // GC_ERR_ACCESS_DENIED almost always means another process (another instance
            // of this app, Arena Capture, IpConfigUtility, etc.) already has the camera
            // open.  It can also appear briefly after an unclean disconnect while the
            // driver is releasing the device.
            errorMsg =
                "Camera is in use by another application.\n\n"
                "Close any other programs that may have the camera open "
                "(e.g. Arena Capture, a second instance of this app, or "
                "IpConfigUtility), then try connecting again.\n\n"
                "If no other app is open, unplug and re-plug the camera "
                "cable, wait a few seconds, and retry.";
        }
        else if (raw.find("GC_ERR_TIMEOUT") != std::string::npos
                 || raw.find("timeout")     != std::string::npos)
        {
            errorMsg =
                "Connection timed out.\n\n"
                "Check that the camera is powered on and the network cable "
                "is securely connected, then try again.";
        }
        else if (raw.find("GC_ERR_NOT_INITIALIZED") != std::string::npos
                 || raw.find("not initialized")      != std::string::npos)
        {
            errorMsg =
                "Camera driver is not ready.\n\n"
                "Try refreshing the camera list. If the problem persists, "
                "restart the application.";
        }
        else if (raw.find("GC_ERR_NOT_FOUND") != std::string::npos
                 || raw.find("not found")      != std::string::npos)
        {
            errorMsg =
                "Camera not found.\n\n"
                "The camera may have been disconnected after the last refresh. "
                "Click Refresh to re-scan for cameras, then try again.";
        }
        else
        {
            // Unknown error — the camera IS in the discovery list but CreateDevice
            // failed.  This often means an IP subnet mismatch (camera rebooted to a
            // link-local address while the NIC is on a different subnet).
            // Try ForceIP to reassign the camera to a compatible address, then retry.
            std::string forceStatus;
            if (tryForceIpConnect(deviceMac, forceStatus))
            {
                return true;   // m_pDevice was set inside tryForceIpConnect
            }

            // ForceIP either wasn't attempted or didn't fix it — show both errors.
            errorMsg = std::string("Connect error: ") + raw;
            if (!forceStatus.empty())
                errorMsg += "\n\nForceIP recovery attempted: " + forceStatus;
        }

        m_pDevice = nullptr;
        return false;
    }
    catch (const std::exception& e)
    {
        errorMsg = std::string("Connect error: ") + e.what();
        m_pDevice = nullptr;
        return false;
    }
}


// =============================================================================
// getUpdatedCameraInfo
// =============================================================================
//
// Rebuilds a CameraInfo from the current m_deviceInfoList (which is refreshed
// by tryForceIpConnect after a ForceIP operation).  MainWindow calls this after
// connectCamera() succeeds to obtain the real IP address — which may differ from
// what was shown at discovery time if ForceIP reassigned it.
bool CameraManager::getUpdatedCameraInfo(const std::string& serialNumber, CameraInfo& info) const
{
    for (Arena::DeviceInfo devInfo : m_deviceInfoList)
    {
        if (std::string(devInfo.SerialNumber().c_str()) != serialNumber)
            continue;

        info.modelName    = std::string(devInfo.ModelName().c_str());
        info.serialNumber = serialNumber;
        info.ipAddress    = std::string(devInfo.IpAddressStr().c_str());
        info.macAddress   = std::string(devInfo.MacAddressStr().c_str());
        info.displayName  = info.modelName + "  (SN: " + info.serialNumber + ")"
                          + "  [" + info.ipAddress + "]";
        return true;
    }
    return false;
}


// =============================================================================
// disconnectCamera
// =============================================================================
void CameraManager::disconnectCamera()
{
    if (m_pDevice != nullptr && m_pSystem != nullptr)
    {
        try
        {
            // DestroyDevice() closes the connection to the camera and frees
            // the IDevice object. After this call, m_pDevice is a dangling pointer
            // (points to freed memory), so we immediately set it to nullptr.
            m_pSystem->DestroyDevice(m_pDevice);
        }
        catch (...) { /* Ignore errors during cleanup */ }

        m_pDevice = nullptr;
    }
}


// =============================================================================
// stopStream
// =============================================================================
void CameraManager::stopStream()
{
    if (m_pDevice)
    {
        try { m_pDevice->StopStream(); } catch (...) {}
    }
}


// =============================================================================
// isConnected
// =============================================================================
bool CameraManager::isConnected() const
{
    return m_pDevice != nullptr;
}


// =============================================================================
// readParameters
// =============================================================================
//
// The Arena SDK uses the GenICam "node map" pattern for camera parameters.
// Each camera exposes a node map — essentially a dictionary of named parameters.
// Each parameter is a "node" with a type (float, integer, enum, bool, string, command).
//
// We use helper functions (getDoubleValue, getInt64Value, etc.) to safely read
// each node, catching errors for nodes that might not exist on all cameras.
bool CameraManager::readParameters(CameraParameters& params, std::string& errorMsg)
{
    if (m_pDevice == nullptr)
    {
        errorMsg = "No camera connected.";
        return false;
    }

    try
    {
        // --- Exposure ---
        params.exposureAutoOptions = getEnumOptions("ExposureAuto");
        params.exposureAuto        = getEnumValue("ExposureAuto");

        // To read range (min/max) of a float node, we need the typed node pointer.
        // GetNodeMap() returns the device's main parameter map.
        // GetNode() finds a node by name, returning a generic INode pointer.
        // We then cast it to a more specific type (CFloatPtr = pointer to float node).
        GenApi::CFloatPtr pExposureTime = m_pDevice->GetNodeMap()->GetNode("ExposureTime");
        if (pExposureTime && GenApi::IsReadable(pExposureTime))
        {
            params.exposureTime    = pExposureTime->GetValue();
            params.exposureTimeMin = pExposureTime->GetMin();
            params.exposureTimeMax = pExposureTime->GetMax();
        }
        else
        {
            params.exposureTime    = 10000.0;  // Fallback: 10 ms
            params.exposureTimeMin = 100.0;
            params.exposureTimeMax = 1000000.0;
        }

        // --- Gain ---
        params.gainAutoOptions = getEnumOptions("GainAuto");
        params.gainAuto        = getEnumValue("GainAuto");

        GenApi::CFloatPtr pGain = m_pDevice->GetNodeMap()->GetNode("Gain");
        if (pGain && GenApi::IsReadable(pGain))
        {
            params.gain    = pGain->GetValue();
            params.gainMin = pGain->GetMin();
            params.gainMax = pGain->GetMax();
        }
        else
        {
            params.gain    = 0.0;
            params.gainMin = 0.0;
            params.gainMax = 24.0;
        }

        // --- Width ---
        GenApi::CIntegerPtr pWidth = m_pDevice->GetNodeMap()->GetNode("Width");
        if (pWidth && GenApi::IsReadable(pWidth))
        {
            params.width    = pWidth->GetValue();
            params.widthMin = pWidth->GetMin();
            params.widthMax = pWidth->GetMax();
            params.widthInc = pWidth->GetInc();  // GetInc() = the step/increment size
        }
        else
        {
            params.width    = 1920;
            params.widthMin = 1;
            params.widthMax = 4096;
            params.widthInc = 1;
        }

        // --- Height ---
        GenApi::CIntegerPtr pHeight = m_pDevice->GetNodeMap()->GetNode("Height");
        if (pHeight && GenApi::IsReadable(pHeight))
        {
            params.height    = pHeight->GetValue();
            params.heightMin = pHeight->GetMin();
            params.heightMax = pHeight->GetMax();
            params.heightInc = pHeight->GetInc();
        }
        else
        {
            params.height    = 1200;
            params.heightMin = 1;
            params.heightMax = 3000;
            params.heightInc = 1;
        }

        // --- Frame Rate ---
        // Not all cameras expose AcquisitionFrameRate as a writable node,
        // so we check availability first.
        params.frameRateAvailable = isNodeAvailable("AcquisitionFrameRate");
        if (params.frameRateAvailable)
        {
            GenApi::CFloatPtr pFPS = m_pDevice->GetNodeMap()->GetNode("AcquisitionFrameRate");
            if (pFPS && GenApi::IsReadable(pFPS))
            {
                params.frameRate    = pFPS->GetValue();
                params.frameRateMin = pFPS->GetMin();
                params.frameRateMax = pFPS->GetMax();
            }
        }
        else
        {
            params.frameRate    = 0.0;
            params.frameRateMin = 0.0;
            params.frameRateMax = 0.0;
        }

        // --- Pixel Format ---
        params.pixelFormatOptions = getEnumOptions("PixelFormat");
        params.pixelFormat        = getEnumValue("PixelFormat");

        return true;
    }
    catch (const GenICam::GenericException& e)
    {
        errorMsg = std::string("Error reading parameters: ") + e.what();
        return false;
    }
    catch (const std::exception& e)
    {
        errorMsg = std::string("Error reading parameters: ") + e.what();
        return false;
    }
}


// =============================================================================
// applyParameters
// =============================================================================
bool CameraManager::applyParameters(const CameraParameters& params, std::string& errorMsg)
{
    if (m_pDevice == nullptr)
    {
        errorMsg = "No camera connected.";
        return false;
    }

    try
    {
        // ORDER MATTERS: Set Auto modes before their manual counterparts.
        // For example, if ExposureAuto is "Continuous", writing ExposureTime might fail
        // because the camera is controlling exposure automatically.

        // --- Pixel Format ---
        // Set pixel format first — it affects the valid range of other parameters.
        GenApi::INodeMap* pNodeMap = m_pDevice->GetNodeMap();

        GenApi::CEnumerationPtr pPixFmt = pNodeMap->GetNode("PixelFormat");
        if (pPixFmt && GenApi::IsWritable(pPixFmt))
        {
            // SetIntValue selects the enum entry by its symbolic name.
            // GetEntryByName("Mono8") finds the entry named "Mono8" and returns its value.
            GenApi::CEnumEntryPtr pEntry = pPixFmt->GetEntryByName(params.pixelFormat.c_str());
            if (pEntry && GenApi::IsReadable(pEntry))
            {
                pPixFmt->SetIntValue(pEntry->GetValue());
            }
        }

        // --- Width & Height ---
        // Some cameras require stopping the stream to change these; we don't enforce
        // that here, but the user should stop acquisition before applying.
        GenApi::CIntegerPtr pWidth = pNodeMap->GetNode("Width");
        if (pWidth && GenApi::IsWritable(pWidth))
        {
            // Clamp to the node's allowed range before writing
            int64_t clamped = std::max<int64_t>(pWidth->GetMin(),
                              std::min<int64_t>(pWidth->GetMax(), params.width));
            pWidth->SetValue(clamped);
        }

        GenApi::CIntegerPtr pHeight = pNodeMap->GetNode("Height");
        if (pHeight && GenApi::IsWritable(pHeight))
        {
            int64_t clamped = std::max<int64_t>(pHeight->GetMin(),
                              std::min<int64_t>(pHeight->GetMax(), params.height));
            pHeight->SetValue(clamped);
        }

        // --- Exposure Auto ---
        GenApi::CEnumerationPtr pExpAuto = pNodeMap->GetNode("ExposureAuto");
        if (pExpAuto && GenApi::IsWritable(pExpAuto))
        {
            GenApi::CEnumEntryPtr pEntry = pExpAuto->GetEntryByName(params.exposureAuto.c_str());
            if (pEntry && GenApi::IsReadable(pEntry))
            {
                pExpAuto->SetIntValue(pEntry->GetValue());
            }
        }

        // --- Exposure Time (only when auto is Off) ---
        if (params.exposureAuto == "Off")
        {
            GenApi::CFloatPtr pExpTime = pNodeMap->GetNode("ExposureTime");
            if (pExpTime && GenApi::IsWritable(pExpTime))
            {
                double clamped = std::max<int64_t>(pExpTime->GetMin(),
                                 std::min<int64_t>(pExpTime->GetMax(), params.exposureTime));
                pExpTime->SetValue(clamped);
            }
        }

        // --- Gain Auto ---
        GenApi::CEnumerationPtr pGainAuto = pNodeMap->GetNode("GainAuto");
        if (pGainAuto && GenApi::IsWritable(pGainAuto))
        {
            GenApi::CEnumEntryPtr pEntry = pGainAuto->GetEntryByName(params.gainAuto.c_str());
            if (pEntry && GenApi::IsReadable(pEntry))
            {
                pGainAuto->SetIntValue(pEntry->GetValue());
            }
        }

        // --- Gain (only when auto is Off) ---
        if (params.gainAuto == "Off")
        {
            GenApi::CFloatPtr pGain = pNodeMap->GetNode("Gain");
            if (pGain && GenApi::IsWritable(pGain))
            {
                double clamped = std::max<int64_t>(pGain->GetMin(),
                                 std::min<int64_t>(pGain->GetMax(), params.gain));
                pGain->SetValue(clamped);
            }
        }

        // --- Frame Rate ---
        if (params.frameRateAvailable)
        {
            // On many cameras, AcquisitionFrameRateEnable must be set to true
            // before AcquisitionFrameRate becomes writable.
            GenApi::CBooleanPtr pFRateEnable = pNodeMap->GetNode("AcquisitionFrameRateEnable");
            if (pFRateEnable && GenApi::IsWritable(pFRateEnable))
            {
                pFRateEnable->SetValue(true);
            }

            GenApi::CFloatPtr pFRate = pNodeMap->GetNode("AcquisitionFrameRate");
            if (pFRate && GenApi::IsWritable(pFRate))
            {
                double clamped = std::max<int64_t>(pFRate->GetMin(),
                                 std::min<int64_t>(pFRate->GetMax(), params.frameRate));
                pFRate->SetValue(clamped);
            }
        }

        return true;
    }
    catch (const GenICam::GenericException& e)
    {
        errorMsg = std::string("Error applying parameters: ") + e.what();
        return false;
    }
    catch (const std::exception& e)
    {
        errorMsg = std::string("Error applying parameters: ") + e.what();
        return false;
    }
}


// =============================================================================
// PRIVATE HELPER METHODS
// =============================================================================


// =============================================================================
// getEnumOptions — get all valid symbolic names for an enum node
// =============================================================================
std::vector<std::string> CameraManager::getEnumOptions(const std::string& nodeName)
{
    std::vector<std::string> options;

    if (m_pDevice == nullptr) return options;

    try
    {
        // Cast the node to CEnumerationPtr — the typed pointer for enum nodes
        GenApi::CEnumerationPtr pEnum = m_pDevice->GetNodeMap()->GetNode(nodeName.c_str());

        if (!pEnum || !GenApi::IsReadable(pEnum))
        {
            return options;  // Node doesn't exist or isn't readable
        }

        // GetEntries() fills a list with all entries in this enumeration.
        // NodeList_t is just a typedef for vector<INode*>.
        GenApi::NodeList_t entries;
        pEnum->GetEntries(entries);

        for (const auto& pNode : entries)
        {
            // Cast each entry to a CEnumEntryPtr to access its symbolic name
            GenApi::CEnumEntryPtr pEntry(pNode);

            // Only include entries that are actually available (some may be unavailable
            // depending on other camera settings)
            if (pEntry && GenApi::IsReadable(pEntry))
            {
                // GetSymbolic() returns the human-readable name like "Continuous"
                options.push_back(std::string(pEntry->GetSymbolic().c_str()));
            }
        }
    }
    catch (...) { /* Return whatever we collected */ }

    return options;
}


// =============================================================================
// getEnumValue — get current symbolic value of an enum node
// =============================================================================
std::string CameraManager::getEnumValue(const std::string& nodeName)
{
    if (m_pDevice == nullptr) return "";

    try
    {
        GenApi::CEnumerationPtr pEnum = m_pDevice->GetNodeMap()->GetNode(nodeName.c_str());

        if (!pEnum || !GenApi::IsReadable(pEnum))
        {
            return "";
        }

        // GetCurrentEntry() returns the currently selected enum entry.
        // GetSymbolic() converts it to the symbolic string name.
        return std::string(pEnum->GetCurrentEntry()->GetSymbolic().c_str());
    }
    catch (...)
    {
        return "";
    }
}


// =============================================================================
// getDoubleValue — safely read a float node
// =============================================================================
double CameraManager::getDoubleValue(const std::string& nodeName, double fallback)
{
    if (m_pDevice == nullptr) return fallback;

    try
    {
        GenApi::CFloatPtr pFloat = m_pDevice->GetNodeMap()->GetNode(nodeName.c_str());
        if (pFloat && GenApi::IsReadable(pFloat))
        {
            return pFloat->GetValue();
        }
    }
    catch (...) {}

    return fallback;
}


// =============================================================================
// getInt64Value — safely read an integer node
// =============================================================================
int64_t CameraManager::getInt64Value(const std::string& nodeName, int64_t fallback)
{
    if (m_pDevice == nullptr) return fallback;

    try
    {
        GenApi::CIntegerPtr pInt = m_pDevice->GetNodeMap()->GetNode(nodeName.c_str());
        if (pInt && GenApi::IsReadable(pInt))
        {
            return pInt->GetValue();
        }
    }
    catch (...) {}

    return fallback;
}


// =============================================================================
// snapInt64Value — round rawValue to the nearest step the camera will accept
// =============================================================================
//
// GenICam integer nodes have three constraints: Min, Max, and Inc (increment).
// The camera rejects any value where (value - min) % inc != 0.
// This helper reads those constraints and returns the closest valid value,
// which PreviewDialog uses after the user draws an ROI rectangle.
int64_t CameraManager::snapInt64Value(const std::string& nodeName, int64_t rawValue)
{
    if (m_pDevice == nullptr) return rawValue;

    try
    {
        GenApi::CIntegerPtr pInt = m_pDevice->GetNodeMap()->GetNode(nodeName.c_str());
        if (pInt && GenApi::IsReadable(pInt))
        {
            int64_t minVal = pInt->GetMin();
            int64_t maxVal = pInt->GetMax();
            int64_t inc    = pInt->GetInc();
            if (inc <= 0) inc = 1;  // Guard against bad camera metadata

            // Clamp raw value into [min, max] before rounding so integer division
            // doesn't produce negative or out-of-range results.
            int64_t clamped = rawValue;
            if (clamped < minVal) clamped = minVal;
            if (clamped > maxVal) clamped = maxVal;

            // Round to nearest multiple of inc above min.
            // (offset + inc/2) / inc performs round-to-nearest via integer arithmetic.
            int64_t offset  = clamped - minVal;
            int64_t snapped = minVal + ((offset + inc / 2) / inc) * inc;

            // If rounding UP pushed past max, floor-round instead.
            // Clamping back to maxVal is wrong when maxVal itself is not on a valid
            // step (e.g. some cameras report WidthMax = 2660 with Min=32, Inc=8;
            // (2660-32)%8 != 0, so we must return 2656, not 2660).
            if (snapped > maxVal)
                snapped = minVal + (offset / inc) * inc;   // largest valid value <= max

            return snapped;
        }
    }
    catch (...) {}

    return rawValue;
}


// =============================================================================
// snapInt64ValueWithMax — like snapInt64Value but with a caller-supplied max
// =============================================================================
//
// WHY THIS EXISTS:
//   OffsetX.GetMax() = SensorWidth - CurrentWidth.  When the camera is streaming
//   at full width, OffsetX.Max = 0, so snapInt64Value("OffsetX", 300) returns 0.
//   When drawing an ROI we want to snap OffsetX relative to the TARGET Width, not
//   the current Width.  The caller computes (SensorWidth - targetWidth) and passes
//   it as overrideMax so we get the correct valid range.
int64_t CameraManager::snapInt64ValueWithMax(const std::string& nodeName,
                                              int64_t rawValue,
                                              int64_t overrideMax)
{
    if (m_pDevice == nullptr) return rawValue;

    try
    {
        GenApi::CIntegerPtr pInt = m_pDevice->GetNodeMap()->GetNode(nodeName.c_str());
        if (pInt && GenApi::IsReadable(pInt))
        {
            int64_t minVal = pInt->GetMin();
            int64_t maxVal = overrideMax;   // Use caller's max, not the live node max
            int64_t inc    = pInt->GetInc();
            if (inc <= 0) inc = 1;

            int64_t clamped = rawValue;
            if (clamped < minVal) clamped = minVal;
            if (clamped > maxVal) clamped = maxVal;

            int64_t offset  = clamped - minVal;
            int64_t snapped = minVal + ((offset + inc / 2) / inc) * inc;

            if (snapped > maxVal)
                snapped = minVal + (offset / inc) * inc;   // floor to largest valid value <= max
            return snapped;
        }
    }
    catch (...) {}

    return rawValue;
}


// =============================================================================
// isNodeAvailable — check if a node exists and is readable
// =============================================================================
bool CameraManager::isNodeAvailable(const std::string& nodeName)
{
    if (m_pDevice == nullptr) return false;

    try
    {
        GenApi::CNodePtr pNode = m_pDevice->GetNodeMap()->GetNode(nodeName.c_str());
        return pNode && GenApi::IsReadable(pNode);
    }
    catch (...)
    {
        return false;
    }
}


// =============================================================================
// NODE WRITING HELPERS
// =============================================================================
//
// These are thin wrappers around the GenICam typed node pointers.
// Each function:
//   1. Guards against a disconnected camera
//   2. Retrieves the named node from the node map
//   3. Verifies it is writable
//   4. Writes the value (with clamping where applicable)
//   5. Catches all GenICam/std exceptions and reports them via errorMsg
//
// C++ CONCEPT — GenICam typed pointers:
//   CEnumerationPtr, CFloatPtr, CIntegerPtr, CBooleanPtr are "smart pointer"
//   wrappers that provide type-safe access to a specific kind of GenICam node.
//   Assigning a generic CNodePtr to them performs a dynamic cast internally —
//   if the node is actually a different type, the pointer becomes null.
// =============================================================================


// =============================================================================
// getNodeEnumString — read the current symbolic value of an enumeration node
// =============================================================================
QString CameraManager::getNodeEnumString(const std::string& nodeName) const
{
    if (!m_pDevice) return {};

    try
    {
        GenApi::CEnumerationPtr pEnum =
            m_pDevice->GetNodeMap()->GetNode(nodeName.c_str());
        if (!pEnum || !GenApi::IsReadable(pEnum)) return {};
        return QString::fromLatin1(
            pEnum->GetCurrentEntry()->GetSymbolic().c_str());
    }
    catch (...) { return {}; }
}


// =============================================================================
// setNodeEnumValue — write a symbolic name to an enumeration node
// =============================================================================
bool CameraManager::setNodeEnumValue(const std::string& nodeName,
                                     const std::string& value,
                                     std::string&       errorMsg)
{
    if (m_pDevice == nullptr)
    {
        errorMsg = "No camera connected.";
        return false;
    }

    try
    {
        GenApi::CEnumerationPtr pEnum = m_pDevice->GetNodeMap()->GetNode(nodeName.c_str());

        if (!pEnum || !GenApi::IsWritable(pEnum))
        {
            errorMsg = "Node '" + nodeName + "' is not writable or does not exist.";
            return false;
        }

        // GetEntryByName() looks up the enum entry by its symbolic name (e.g., "Off").
        // If the name is invalid, it returns a null pointer.
        GenApi::CEnumEntryPtr pEntry = pEnum->GetEntryByName(value.c_str());
        if (!pEntry || !GenApi::IsReadable(pEntry))
        {
            errorMsg = "Value '" + value + "' is not a valid entry for node '" + nodeName + "'.";
            return false;
        }

        // SetIntValue selects the enum entry by its underlying integer value.
        // GetValue() on an entry returns that integer — this is the standard GenICam pattern.
        pEnum->SetIntValue(pEntry->GetValue());
        return true;
    }
    catch (const GenICam::GenericException& e)
    {
        errorMsg = std::string("Error setting '") + nodeName + "': " + e.what();
        return false;
    }
    catch (const std::exception& e)
    {
        errorMsg = std::string("Error setting '") + nodeName + "': " + e.what();
        return false;
    }
}


// =============================================================================
// setNodeDoubleValue — write a floating-point value to a float node
// =============================================================================
bool CameraManager::setNodeDoubleValue(const std::string& nodeName,
                                       double             value,
                                       std::string&       errorMsg)
{
    if (m_pDevice == nullptr)
    {
        errorMsg = "No camera connected.";
        return false;
    }

    try
    {
        GenApi::CFloatPtr pFloat = m_pDevice->GetNodeMap()->GetNode(nodeName.c_str());

        if (!pFloat || !GenApi::IsWritable(pFloat))
        {
            errorMsg = "Node '" + nodeName + "' is not writable or does not exist.";
            return false;
        }

        // Clamp to the camera's allowed range before writing.
        // Writing an out-of-range value throws a GenICam exception.
        double clamped = std::max<int64_t>(pFloat->GetMin(), std::min<int64_t>(pFloat->GetMax(), value));
        pFloat->SetValue(clamped);
        return true;
    }
    catch (const GenICam::GenericException& e)
    {
        errorMsg = std::string("Error setting '") + nodeName + "': " + e.what();
        return false;
    }
    catch (const std::exception& e)
    {
        errorMsg = std::string("Error setting '") + nodeName + "': " + e.what();
        return false;
    }
}


// =============================================================================
// setNodeInt64Value — write an integer value to an integer node
// =============================================================================
bool CameraManager::setNodeInt64Value(const std::string& nodeName,
                                      int64_t            value,
                                      std::string&       errorMsg)
{
    if (m_pDevice == nullptr)
    {
        errorMsg = "No camera connected.";
        return false;
    }

    try
    {
        GenApi::CIntegerPtr pInt = m_pDevice->GetNodeMap()->GetNode(nodeName.c_str());

        if (!pInt || !GenApi::IsWritable(pInt))
        {
            errorMsg = "Node '" + nodeName + "' is not writable or does not exist.";
            return false;
        }

        int64_t minVal  = pInt->GetMin();
        int64_t maxVal  = pInt->GetMax();
        int64_t inc     = pInt->GetInc();
        if (inc <= 0) inc = 1;

        // Clamp to [min, max] first.
        int64_t clamped = std::max<int64_t>(minVal, std::min<int64_t>(maxVal, value));

        // Snap to the nearest valid increment.
        // The camera requires (value - min) % inc == 0; clamping alone doesn't guarantee this.
        // We floor-divide so the result always stays <= clamped and within [min, max].
        if (inc > 1)
        {
            int64_t offset  = clamped - minVal;
            clamped = minVal + (offset / inc) * inc;   // largest valid step <= clamped
        }

        pInt->SetValue(clamped);
        return true;
    }
    catch (const GenICam::GenericException& e)
    {
        errorMsg = std::string("Error setting '") + nodeName + "': " + e.what();
        return false;
    }
    catch (const std::exception& e)
    {
        errorMsg = std::string("Error setting '") + nodeName + "': " + e.what();
        return false;
    }
}


// =============================================================================
// setNodeBoolValue — write a boolean value to a boolean node
// =============================================================================
bool CameraManager::setNodeBoolValue(const std::string& nodeName,
                                     bool               value,
                                     std::string&       errorMsg)
{
    if (m_pDevice == nullptr)
    {
        errorMsg = "No camera connected.";
        return false;
    }

    try
    {
        GenApi::CBooleanPtr pBool = m_pDevice->GetNodeMap()->GetNode(nodeName.c_str());

        if (!pBool || !GenApi::IsWritable(pBool))
        {
            errorMsg = "Node '" + nodeName + "' is not writable or does not exist.";
            return false;
        }

        pBool->SetValue(value);
        return true;
    }
    catch (const GenICam::GenericException& e)
    {
        errorMsg = std::string("Error setting '") + nodeName + "': " + e.what();
        return false;
    }
    catch (const std::exception& e)
    {
        errorMsg = std::string("Error setting '") + nodeName + "': " + e.what();
        return false;
    }
}


// =============================================================================
// tryForceIpConnect (private)
// =============================================================================
//
// Called by connectCamera() when CreateDevice() fails with an unrecognised error.
// The most common cause is an IP subnet mismatch: the camera rebooted to a
// link-local address (169.254.x.x) while the host NIC is on a routable subnet,
// so the GigEVision driver can discover the camera via L2 broadcast but cannot
// open a data channel to it.
//
// Strategy:
//   1. Find the camera's current IP from the cached discovery list (by MAC).
//   2. Enumerate local NICs via GetAdaptersInfo; pick the best routable one.
//   3. Build a target IP on the NIC's subnet using the camera's last octet.
//   4. Call ForceIp(), wait, rediscover, then CreateDevice().
//
// Returns true and sets m_pDevice on success.
// Returns false and describes what was attempted in statusMsg.
bool CameraManager::tryForceIpConnect(uint64_t mac, std::string& statusMsg)
{
    if (!m_pSystem) { statusMsg = "No system."; return false; }

    // --- 1. Find camera in the cached discovery list by MAC ---
    auto camIt = std::find_if(
        m_deviceInfoList.begin(), m_deviceInfoList.end(),
        [mac](Arena::DeviceInfo& d) { return d.MacAddress() == mac; });

    if (camIt == m_deviceInfoList.end())
    {
        statusMsg = "Camera not found in device list by MAC.";
        return false;
    }

    const uint32_t camCurrentIp = camIt->IpAddress();  // host byte order, MSB = first octet

    // --- 2. Enumerate local NICs ---
    ULONG bufLen = 16 * 1024;
    std::vector<BYTE> buf(bufLen, 0);
    DWORD ret = GetAdaptersInfo(reinterpret_cast<IP_ADAPTER_INFO*>(buf.data()), &bufLen);
    if (ret == ERROR_BUFFER_OVERFLOW)
    {
        buf.assign(bufLen, 0);
        ret = GetAdaptersInfo(reinterpret_cast<IP_ADAPTER_INFO*>(buf.data()), &bufLen);
    }
    if (ret != NO_ERROR)
    {
        statusMsg = "GetAdaptersInfo failed (error " + std::to_string(ret) + ").";
        return false;
    }

    // Parse dotted-decimal string (e.g. "192.168.1.100") into host-byte-order uint32_t.
    auto parseIp = [](const char* s, uint32_t& out) -> bool
    {
        unsigned int o1, o2, o3, o4;
        if (sscanf_s(s, "%u.%u.%u.%u", &o1, &o2, &o3, &o4) != 4) return false;
        out = (o1 << 24) | (o2 << 16) | (o3 << 8) | o4;
        return true;
    };

    // --- 2b. Collect all candidate NICs, scored by subnet preference ---
    // Private subnets (10/172.16-31/192.168) are tried before public IPs
    // because a camera NIC is almost always on a private address.
    // Link-local (169.254.x.x) is tried last as a fallback.
    struct NicCandidate { uint32_t ip, mask, gw; int score; };
    std::vector<NicCandidate> candidates;

    const IP_ADAPTER_INFO* pHead = reinterpret_cast<const IP_ADAPTER_INFO*>(buf.data());
    for (const IP_ADAPTER_INFO* p = pHead; p != nullptr; p = p->Next)
    {
        uint32_t ip = 0, mask = 0, gw = 0;
        if (!parseIp(p->IpAddressList.IpAddress.String, ip))   continue;
        if (!parseIp(p->IpAddressList.IpMask.String,   mask))  continue;
        parseIp(p->GatewayList.IpAddress.String, gw);

        if (ip == 0 || mask == 0) continue;
        if ((ip & 0xFF000000) == 0x7F000000) continue;   // loopback

        int score = 0;
        if      ((ip & 0xFFFF0000) == 0xA9FE0000) score = 1;  // 169.254 link-local
        else if ((ip & 0xFF000000) == 0x0A000000) score = 3;  // 10.x
        else if ((ip & 0xFFF00000) == 0xAC100000) score = 3;  // 172.16-31.x
        else if ((ip & 0xFFFF0000) == 0xC0A80000) score = 4;  // 192.168.x.x  ← camera NIC
        else                                       score = 2;  // public routable

        candidates.push_back({ip, mask, gw, score});
    }

    if (candidates.empty())
    {
        statusMsg = "No suitable local NIC found for ForceIP recovery.";
        return false;
    }

    // Highest score first.
    std::sort(candidates.begin(), candidates.end(),
              [](const NicCandidate& a, const NicCandidate& b){ return a.score > b.score; });

    auto fmtIp = [](uint32_t ip) -> std::string
    {
        return std::to_string((ip >> 24) & 0xFF) + "." +
               std::to_string((ip >> 16) & 0xFF) + "." +
               std::to_string((ip >>  8) & 0xFF) + "." +
               std::to_string( ip        & 0xFF);
    };

    uint8_t lastOctet = static_cast<uint8_t>(camCurrentIp & 0xFF);
    if (lastOctet == 0 || lastOctet == 255) lastOctet = 100;

    std::string attempts;
    for (const NicCandidate& nic : candidates)
    {
        // --- 3. Compute target IP: NIC network prefix | camera's last octet ---
        const uint32_t nicNet  = nic.ip & nic.mask;
        uint32_t targetIp = nicNet | lastOctet;
        if (targetIp == nic.ip)
            targetIp = nicNet | (lastOctet < 254u ? lastOctet + 1u : 1u);

        const std::string attempt = fmtIp(nic.ip) + " → " + fmtIp(targetIp) + "/" + fmtIp(nic.mask);

        // --- 4. Issue ForceIP ---
        try { m_pSystem->ForceIp(mac, targetIp, nic.mask, nic.gw); }
        catch (const GenICam::GenericException& e)
        {
            attempts += "\n  " + attempt + " — ForceIp threw: " + e.what();
            continue;
        }

        // --- 5. Rediscover ---
        try { m_pSystem->UpdateDevices(1000); } catch (...) {}
        try { m_deviceInfoList = m_pSystem->GetDevices(); } catch (...) {}

        auto newIt = std::find_if(
            m_deviceInfoList.begin(), m_deviceInfoList.end(),
            [mac](Arena::DeviceInfo& d) { return d.MacAddress() == mac; });

        if (newIt == m_deviceInfoList.end())
        {
            attempts += "\n  " + attempt + " — not found after rediscovery";
            continue;
        }

        // --- 6. Retry CreateDevice ---
        try
        {
            m_pDevice = m_pSystem->CreateDevice(*newIt);
            if (m_pDevice)
            {
                statusMsg = "ForceIP succeeded via NIC " + attempt;
                return true;
            }
            attempts += "\n  " + attempt + " — CreateDevice returned null";
        }
        catch (const GenICam::GenericException& e)
        {
            attempts += "\n  " + attempt + " — CreateDevice failed: " + e.what();
            m_pDevice = nullptr;
        }
    }

    statusMsg = "ForceIP recovery tried all NICs, none succeeded:" + attempts;
    return false;
}
