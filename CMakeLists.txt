cmake_minimum_required(VERSION 3.25)
project(borderlessWindowFixes C CXX ASM_MASM)

if(NOT MSVC)
  message(FATAL_ERROR "borderless-window-fixes builds Windows DLLs with MSVC; configure it from a Visual Studio environment (build.ps1 does)")
endif()
if(gameArchitecture STREQUAL "x64")
  set(expectedPointerSize 8)
elseif(gameArchitecture STREQUAL "x86")
  set(expectedPointerSize 4)
else()
  message(FATAL_ERROR "gameArchitecture must be x64 or x86 (build.ps1 sets it from game.cfg)")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL expectedPointerSize)
  message(FATAL_ERROR "the compiler builds for a different architecture than ${gameArchitecture}; configure from the Visual Studio ${gameArchitecture} environment (build.ps1 does)")
endif()
if(NOT DEFINED game)
  message(FATAL_ERROR "game is not set (build.ps1 sets it from -game)")
endif()
set(gameDirectory "${CMAKE_SOURCE_DIR}/games/${game}")
if(NOT EXISTS "${gameDirectory}/game.cfg")
  message(FATAL_ERROR "games/${game}/game.cfg does not exist")
endif()
if(NOT DEFINED gameExecutable)
  message(FATAL_ERROR "gameExecutable is not set (build.ps1 sets it from game.cfg)")
endif()
if(NOT DEFINED windowFixesBuildCommit)
  message(FATAL_ERROR "windowFixesBuildCommit is not set (build.ps1 sets it from git)")
endif()

# Flags are set per target below, so CMake's defaults (exceptions on, a per-configuration
# optimization level) do not fight them.
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  message(FATAL_ERROR "configure with -DCMAKE_BUILD_TYPE=Release (build.ps1 does)")
endif()
set(CMAKE_C_FLAGS "/DWIN32 /D_WINDOWS")
set(CMAKE_CXX_FLAGS "/DWIN32 /D_WINDOWS")
set(CMAKE_C_FLAGS_RELEASE "")
set(CMAKE_CXX_FLAGS_RELEASE "")
# The static runtime leaves DLLs every Windows install already has as the only dependencies.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(outputDirectory "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${outputDirectory}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${outputDirectory}")
# Import libraries go elsewhere: the loader's version.lib in the link directory would shadow
# the Windows SDK's.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

set(nativeDirectory "${CMAKE_SOURCE_DIR}/native")
set(minhookDirectory "${nativeDirectory}/shared/vendor/minhook")
add_library(minhook STATIC
  "${minhookDirectory}/src/buffer.c"
  "${minhookDirectory}/src/hook.c"
  "${minhookDirectory}/src/trampoline.c"
  "${minhookDirectory}/src/hde/hde$<IF:$<STREQUAL:${gameArchitecture},x64>,64,32>.c")
target_include_directories(minhook PUBLIC "${minhookDirectory}/include" PRIVATE "${minhookDirectory}/src")
target_compile_options(minhook PRIVATE /O2 /Gy /Brepro)

file(GLOB sharedSources CONFIGURE_DEPENDS "${nativeDirectory}/shared/source/*.cpp")

# embedDefaultFiles(<toolName> <settings file>...): the tool carries its settings files, embedded at build
# time, and writes one out when the windowFixes folder lacks it, so a release ships no settings files.
function(embedDefaultFiles toolName)
  set(outputPath "${CMAKE_BINARY_DIR}/generated/${toolName}DefaultFiles.cpp")
  list(LENGTH ARGN defaultFileCount)
  set(fileArguments "")
  set(index 0)
  foreach(sourcePath IN LISTS ARGN)
    list(APPEND fileArguments "-DdefaultFile${index}=${sourcePath}")
    math(EXPR index "${index} + 1")
  endforeach()
  add_custom_command(OUTPUT "${outputPath}"
    COMMAND "${CMAKE_COMMAND}" "-DoutputPath=${outputPath}" "-DdefaultFileCount=${defaultFileCount}" ${fileArguments}
      -P "${CMAKE_SOURCE_DIR}/cmake/embedDefaultFiles.cmake"
    DEPENDS ${ARGN} "${CMAKE_SOURCE_DIR}/cmake/embedDefaultFiles.cmake"
    VERBATIM)
  target_sources(${toolName} PRIVATE "${outputPath}")
endfunction()

# addToolLibrary(<toolName> <outputName> <toolFolder> [SOURCES <extra source>...] [SETTINGS <extra settings file>...]):
# every tool compiles all of shared/source; function-level linking lets the linker drop what a tool does not use.
# Its settings files are the .cfg files in the tool's folder plus any SETTINGS.
function(addToolLibrary toolName outputName toolFolder)
  cmake_parse_arguments(PARSE_ARGV 3 tool "" "" "SOURCES;SETTINGS")
  file(GLOB toolSources CONFIGURE_DEPENDS "${toolFolder}/source/*.cpp")
  add_library(${toolName} SHARED ${sharedSources} ${toolSources} ${tool_SOURCES})
  set_target_properties(${toolName} PROPERTIES OUTPUT_NAME ${outputName})
  target_include_directories(${toolName} PRIVATE "${nativeDirectory}/shared/source")
  target_link_libraries(${toolName} PRIVATE minhook)
  # /Brepro leaves no timestamps, so the same source builds the same bytes and a release only moves when its code does.
  target_link_options(${toolName} PRIVATE /OPT:REF /OPT:ICF /Brepro)
  # MSVC deprecates the bounded C string functions the code uses in favor of its _s variants.
  target_compile_definitions(${toolName} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _HAS_EXCEPTIONS=0 _CRT_SECURE_NO_WARNINGS
    "windowFixesBuildCommit=\"${windowFixesBuildCommit}\"")
  target_compile_options(${toolName} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX /EHs-c- /GR- /Gy /utf-8 /O2 /Brepro>)
  file(GLOB settingsFiles CONFIGURE_DEPENDS "${toolFolder}/*.cfg")
  embedDefaultFiles(${toolName} ${settingsFiles} ${tool_SETTINGS})
endfunction()

# The loader carries the game's tool list, so each game gets its own build of it.
# x64 has no inline assembly, so its version.dll stubs are versionStubs.asm; x86's are in versionProxy.cpp.
set(versionStubs "")
if(gameArchitecture STREQUAL "x64")
  set(versionStubs "${nativeDirectory}/fixLoader/source/versionStubs.asm")
endif()
addToolLibrary(fixLoader version "${nativeDirectory}/fixLoader"
  SOURCES "${nativeDirectory}/fixLoader/source/exports.def" ${versionStubs}
  SETTINGS "${gameDirectory}/windowFixes.txt")
target_compile_definitions(fixLoader PRIVATE "windowFixesGameExecutable=\"${gameExecutable}\"")

# dbghelp is loaded at runtime from the system folder, so it is not linked.
# Its stack walking reads x64 unwind tables.
if(gameArchitecture STREQUAL "x64")
  addToolLibrary(stabilityGuard stabilityGuard "${nativeDirectory}/stabilityGuard")
  target_link_libraries(stabilityGuard PRIVATE psapi)
endif()

addToolLibrary(displayGuard displayGuard "${nativeDirectory}/displayGuard")

addToolLibrary(borderlessFit borderlessFit "${nativeDirectory}/borderlessFit")

# A game's own tools live in games/<game>/native/<tool>, built the same way.
file(GLOB gameToolFolders LIST_DIRECTORIES true "${gameDirectory}/native/*")
foreach(gameToolFolder IN LISTS gameToolFolders)
  if(IS_DIRECTORY "${gameToolFolder}")
    get_filename_component(gameToolName "${gameToolFolder}" NAME)
    addToolLibrary(${gameToolName} ${gameToolName} "${gameToolFolder}")
  endif()
endforeach()
