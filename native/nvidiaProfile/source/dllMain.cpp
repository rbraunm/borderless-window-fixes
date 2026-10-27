#include "fatalError.h"
#include "sessionLog.h"
#include "settingsFile.h"
#include "toolHost.h"

#include <windows.h>

#pragma warning(push, 0)
#include <nvapi.h>
#include <NvApiDriverSettings.h>
#pragma warning(pop)

#include <cstring>
#include <string>

#ifndef windowFixesBuildCommit
#error "windowFixesBuildCommit must be defined by the build (see build.ps1)"
#endif

static const char* const toolName = "nvidiaProfile";
static const char* const nvidiaProfileVersion = "0.1.0";
static const char* const configurationFileName = "nvidiaProfile.cfg";
static const wchar_t* const createdProfileSuffix = L" (borderless-window-fixes)";

static_assert(sizeof(wchar_t) == sizeof(NvU16), "NVAPI strings are UTF-16");

static bool isLoaded = false;
// Several kilobytes each: kept off the main thread's stack.
static NVDRS_SETTING currentSetting;
static NVDRS_SETTING newSetting;
static NVDRS_APPLICATION application;
static NVDRS_PROFILE profile;

struct PresentMethod {
  const char* name;
  NvU32 value;
};

static const PresentMethod presentMethods[] = {
  { "native", OGL_CPL_PREFER_DXPRESENT_PREFER_DISABLED },
  { "layered", OGL_CPL_PREFER_DXPRESENT_PREFER_ENABLED },
  { "auto", OGL_CPL_PREFER_DXPRESENT_AUTO },
};

static const char* presentMethodName(NvU32 value) {
  for (const PresentMethod& method : presentMethods) {
    if (method.value == value) {
      return method.name;
    }
  }
  return "unknown";
}

static const char* locationName(NVDRS_SETTING_LOCATION location) {
  return location == NVDRS_CURRENT_PROFILE_LOCATION ? "profile" : location == NVDRS_GLOBAL_PROFILE_LOCATION ? "global"
    : location == NVDRS_BASE_PROFILE_LOCATION ? "base" : location == NVDRS_DEFAULT_PROFILE_LOCATION ? "default" : "unknown";
}

static std::string describeStatus(NvAPI_Status status) {
  NvAPI_ShortString message = {};
  NvAPI_GetErrorMessage(status, message);
  return formattedText("%d (%s)", static_cast<int>(status), message);
}

static void requireSuccess(NvAPI_Status status, const char* call) {
  if (status != NVAPI_OK) {
    fatalError("%s failed: %s", call, describeStatus(status).c_str());
  }
}

static std::string narrow(const NvU16* text) {
  const int length = WideCharToMultiByte(CP_ACP, 0, reinterpret_cast<const wchar_t*>(text), -1, nullptr, 0, nullptr, nullptr);
  if (length <= 0) {
    return "";
  }
  std::string converted(static_cast<size_t>(length) - 1, '\0');
  WideCharToMultiByte(CP_ACP, 0, reinterpret_cast<const wchar_t*>(text), -1, &converted[0], length, nullptr, nullptr);
  return converted;
}

static void copyUnicode(NvAPI_UnicodeString destination, const std::wstring& text) {
  if (text.size() >= NVAPI_UNICODE_STRING_MAX) {
    fatalError("Profile text is longer than NVAPI allows: %zu characters", text.size());
  }
  memcpy(destination, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
}

// The profile the driver applies to the running executable, created for it when the driver has none.
static NvDRSProfileHandle findOrCreateProfile(NvDRSSessionHandle session, const std::wstring& executablePath,
    const std::wstring& executableName) {
  NvDRSProfileHandle handle = nullptr;
  memset(&application, 0, sizeof(application));
  application.version = NVDRS_APPLICATION_VER;
  NvAPI_UnicodeString path = {};
  copyUnicode(path, executablePath);
  const NvAPI_Status found = NvAPI_DRS_FindApplicationByName(session, path, &handle, &application);
  if (found == NVAPI_OK) {
    return handle;
  }
  if (found != NVAPI_EXECUTABLE_NOT_FOUND) {
    fatalError("NvAPI_DRS_FindApplicationByName failed: %s", describeStatus(found).c_str());
  }
  memset(&profile, 0, sizeof(profile));
  profile.version = NVDRS_PROFILE_VER;
  copyUnicode(profile.profileName, executableName + createdProfileSuffix);
  requireSuccess(NvAPI_DRS_CreateProfile(session, &profile, &handle), "NvAPI_DRS_CreateProfile");
  memset(&application, 0, sizeof(application));
  application.version = NVDRS_APPLICATION_VER;
  copyUnicode(application.appName, executableName);
  requireSuccess(NvAPI_DRS_CreateApplication(session, handle, &application), "NvAPI_DRS_CreateApplication");
  writeLogLine("INFO", "profileCreated name=%s application=%s", narrow(profile.profileName).c_str(), narrow(application.appName).c_str());
  return handle;
}

static void applyPresentMethod(NvU32 desired) {
  wchar_t executablePath[MAX_PATH];
  const DWORD pathLength = GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
  if (pathLength == 0 || pathLength >= MAX_PATH) {
    fatalError("Could not resolve the game executable path (error %lu)", GetLastError());
  }
  const std::wstring path(executablePath);
  const std::wstring name = path.substr(path.find_last_of(L'\\') + 1);

  NvDRSSessionHandle session = nullptr;
  requireSuccess(NvAPI_DRS_CreateSession(&session), "NvAPI_DRS_CreateSession");
  requireSuccess(NvAPI_DRS_LoadSettings(session), "NvAPI_DRS_LoadSettings");
  NvDRSProfileHandle handle = findOrCreateProfile(session, path, name);
  memset(&profile, 0, sizeof(profile));
  profile.version = NVDRS_PROFILE_VER;
  requireSuccess(NvAPI_DRS_GetProfileInfo(session, handle, &profile), "NvAPI_DRS_GetProfileInfo");

  memset(&currentSetting, 0, sizeof(currentSetting));
  currentSetting.version = NVDRS_SETTING_VER;
  const NvAPI_Status read = NvAPI_DRS_GetSetting(session, handle, OGL_CPL_PREFER_DXPRESENT_ID, &currentSetting);
  if (read != NVAPI_OK && read != NVAPI_SETTING_NOT_FOUND) {
    fatalError("NvAPI_DRS_GetSetting failed: %s", describeStatus(read).c_str());
  }
  const bool isSet = read == NVAPI_OK;
  writeLogLine("INFO", "presentMethodFound profile=%s predefinedProfile=%s value=%s location=%s desired=%s",
    narrow(profile.profileName).c_str(), profile.isPredefined != 0 ? "true" : "false",
    isSet ? presentMethodName(currentSetting.u32CurrentValue) : "notSet", isSet ? locationName(currentSetting.settingLocation) : "none",
    presentMethodName(desired));
  if (isSet && currentSetting.settingLocation == NVDRS_CURRENT_PROFILE_LOCATION && currentSetting.u32CurrentValue == desired) {
    requireSuccess(NvAPI_DRS_DestroySession(session), "NvAPI_DRS_DestroySession");
    return;
  }

  memset(&newSetting, 0, sizeof(newSetting));
  newSetting.version = NVDRS_SETTING_VER;
  newSetting.settingId = OGL_CPL_PREFER_DXPRESENT_ID;
  newSetting.settingType = NVDRS_DWORD_TYPE;
  newSetting.u32CurrentValue = desired;
  requireSuccess(NvAPI_DRS_SetSetting(session, handle, &newSetting), "NvAPI_DRS_SetSetting");
  requireSuccess(NvAPI_DRS_SaveSettings(session), "NvAPI_DRS_SaveSettings");
  requireSuccess(NvAPI_DRS_DestroySession(session), "NvAPI_DRS_DestroySession");
  writeLogLine("INFO", "presentMethodWritten profile=%s value=%s", narrow(profile.profileName).c_str(), presentMethodName(desired));
}

extern "C" {

// fixLoader calls this on the game's main thread at its entry point, before any game code runs and so before the
// game loads OpenGL. The tool has no Unload: the driver reads the profile when OpenGL starts.
__declspec(dllexport) DWORD Load(const ToolHost* host) {
  if (isLoaded) {
    fatalError("Load was called twice");
  }
  isLoaded = true;

  setFatalErrorTitle(toolName);
  openSessionLog(host->toolDirectory, toolName, host->sessionStamp);
  writeLogLine("INFO", "sessionStart version=%s buildCommit=%s mainThreadID=%lu", nvidiaProfileVersion, windowFixesBuildCommit,
    GetCurrentThreadId());

  std::string presentMethodSetting;
  const SettingDefinition definitions[] = {
    { "openglPresentMethod", SettingKind::text, &presentMethodSetting, false },
  };
  loadSettingsFile(host->toolDirectory, configurationFileName, definitions, sizeof(definitions) / sizeof(definitions[0]));
  writeLogLine("INFO", "configurationLoaded openglPresentMethod=%s", presentMethodSetting.c_str());
  if (presentMethodSetting == "unchanged") {
    writeLogLine("INFO", "loadComplete");
    return 0;
  }
  const PresentMethod* desired = nullptr;
  for (const PresentMethod& method : presentMethods) {
    if (presentMethodSetting == method.name) {
      desired = &method;
    }
  }
  if (desired == nullptr) {
    fatalError("%s: openglPresentMethod must be native, layered, auto or unchanged, not %s", configurationFileName,
      presentMethodSetting.c_str());
  }

  const NvAPI_Status initialized = NvAPI_Initialize();
  if (initialized == NVAPI_LIBRARY_NOT_FOUND || initialized == NVAPI_NVIDIA_DEVICE_NOT_FOUND) {
    writeLogLine("WARN", "nvidiaAbsent status=%d", static_cast<int>(initialized));
    writeLogLine("INFO", "loadComplete");
    return 0;
  }
  requireSuccess(initialized, "NvAPI_Initialize");
  applyPresentMethod(desired->value);
  requireSuccess(NvAPI_Unload(), "NvAPI_Unload");
  writeLogLine("INFO", "loadComplete");
  return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}

}
