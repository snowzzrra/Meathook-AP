cmake_minimum_required(VERSION 3.20)
project(MeathookAP LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
  message(FATAL_ERROR "Use cmake/windows-msvc-clang.cmake for the x86_64 Windows MSVC ABI build")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Meathook-AP build requires x86_64; selected target is not 64-bit")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CONFIGURATION_TYPES Release CACHE STRING "" FORCE)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Release-only portability build" FORCE)
endif()

set(MH_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(MH_RPC_DIR "${MH_ROOT}/RPCInterface")
set(MH_M34THOOK_DIR "${MH_ROOT}/m34thook")
set(MH_SNAPHAK_DIR "${MH_ROOT}/snaphak_algo")
set(MH_UDIS_DIR "${MH_ROOT}/udis86test")

set(SNAPHAK_SOURCES
  "${MH_SNAPHAK_DIR}/impls/init_impls_avx2.cpp"
  "${MH_SNAPHAK_DIR}/impls/init_impls_avx512.cpp"
  "${MH_SNAPHAK_DIR}/impls/init_impls_generic.cpp"
  "${MH_SNAPHAK_DIR}/init_snapalgo.cpp"
  "${MH_SNAPHAK_DIR}/snaphakalgo_coro.cpp"
  "${MH_SNAPHAK_DIR}/snaphakalgo_heap.cpp"
  "${MH_SNAPHAK_DIR}/snaphakalgo_networking.cpp"
  "${MH_SNAPHAK_DIR}/snaphakalgo_real.cpp"
  "${MH_SNAPHAK_DIR}/snaphakalgo_smt.cpp"
  "${MH_SNAPHAK_DIR}/snaphakalgo_vmem.cpp"
  "${MH_SNAPHAK_DIR}/syscall_interface.cpp")
set(UDIS_SOURCES
  "${MH_UDIS_DIR}/decode.cpp"
  "${MH_UDIS_DIR}/itab.cpp"
  "${MH_UDIS_DIR}/syn-att.cpp"
  "${MH_UDIS_DIR}/syn-intel.cpp"
  "${MH_UDIS_DIR}/syn.cpp"
  "${MH_UDIS_DIR}/udis86.cpp")
set(MH_M34THOOK_SOURCES
  "${MH_RPC_DIR}/meathook_interface_s.c"
  "${MH_M34THOOK_DIR}/cmdsystem.cpp"
  "${MH_M34THOOK_DIR}/compressedfile_override.cpp"
  "${MH_M34THOOK_DIR}/errorhandling_hooks.cpp"
  "${MH_M34THOOK_DIR}/eventdef.cpp"
  "${MH_M34THOOK_DIR}/fs_hooks.cpp"
  "${MH_M34THOOK_DIR}/gameapi.cpp"
  "${MH_M34THOOK_DIR}/game_exe_interface.cpp"
  "${MH_M34THOOK_DIR}/grab_spawninfo_Command.cpp"
  "${MH_M34THOOK_DIR}/idLib.cpp"
  "${MH_M34THOOK_DIR}/idtypeinfo.cpp"
  "${MH_M34THOOK_DIR}/id_resources_lowlevel.cpp"
  "${MH_M34THOOK_DIR}/main.cpp"
  "${MH_M34THOOK_DIR}/meathook.cpp"
  "${MH_M34THOOK_DIR}/memscan.cpp"
  "${MH_M34THOOK_DIR}/mhgui.cpp"
  "${MH_M34THOOK_DIR}/mhserver.cpp"
  "${MH_M34THOOK_DIR}/mhutil/mh_filesystem.cpp"
  "${MH_M34THOOK_DIR}/mh_cmds_cheats.cpp"
  "${MH_M34THOOK_DIR}/mh_cmds_dump_and_gen.cpp"
  "${MH_M34THOOK_DIR}/mh_cmds_editor.cpp"
  "${MH_M34THOOK_DIR}/mh_cmds_misc_and_dev.cpp"
  "${MH_M34THOOK_DIR}/mh_cmds_queries.cpp"
  "${MH_M34THOOK_DIR}/mh_cmds_spawning.cpp"
  "${MH_M34THOOK_DIR}/mh_dev_hooks.cpp"
  "${MH_M34THOOK_DIR}/mh_editor_mode.cpp"
  "${MH_M34THOOK_DIR}/mh_guirender.cpp"
  "${MH_M34THOOK_DIR}/mh_headergen.cpp"
  "${MH_M34THOOK_DIR}/mh_inputsys.cpp"
  "${MH_M34THOOK_DIR}/mh_kwsearch.cpp"
  "${MH_M34THOOK_DIR}/mh_mainloop.cpp"
  "${MH_M34THOOK_DIR}/mh_ap_runtime.cpp"
  "${MH_M34THOOK_DIR}/mh_memmanip_cmds.cpp"
  "${MH_M34THOOK_DIR}/mh_native_api.cpp"
  "${MH_M34THOOK_DIR}/mh_staticmodel.cpp"
  "${MH_M34THOOK_DIR}/objfiles/objloader.cpp"
  "${MH_M34THOOK_DIR}/optimizations/mhoptimize.cpp"
  "${MH_M34THOOK_DIR}/pregenerated/doom_eternal_cvars_generated.cpp"
  "${MH_M34THOOK_DIR}/pregenerated/doom_eternal_properties_generated.cpp"
  "${MH_M34THOOK_DIR}/rendermodelgui.cpp"
  "${MH_M34THOOK_DIR}/scanners/scanners_avx2.cpp"
  "${MH_M34THOOK_DIR}/scanners/scanners_avx512.cpp"
  "${MH_M34THOOK_DIR}/scanners/scanners_generic.cpp"
  "${MH_M34THOOK_DIR}/scanner_core.cpp"
  "${MH_M34THOOK_DIR}/thunkstrementer.cpp")

foreach(source IN LISTS SNAPHAK_SOURCES UDIS_SOURCES MH_M34THOOK_SOURCES)
  if(NOT EXISTS "${source}")
    message(FATAL_ERROR "Source inventory from current vcxproj is missing: ${source}")
  endif()
endforeach()

file(GLOB SNAPHAK_MASM CONFIGURE_DEPENDS "${MH_SNAPHAK_DIR}/*.asm")
if(NOT SNAPHAK_MASM)
  message(FATAL_ERROR "MASM source inventory is empty; expected snaphak_algo/*.asm")
endif()
set(SNAPHAK_MASM_OBJECTS)
foreach(asm_source IN LISTS SNAPHAK_MASM)
  get_filename_component(asm_name "${asm_source}" NAME_WE)
  set(asm_object "${CMAKE_CURRENT_BINARY_DIR}/masm/${asm_name}.obj")
  add_custom_command(
    OUTPUT "${asm_object}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/masm"
    COMMAND "${MH_LLVM_ML}" -m64 /nologo /c /Fo"${asm_object}" "${asm_source}"
    DEPENDS "${asm_source}"
    COMMENT "Assembling ${asm_name}.asm with ${MH_LLVM_ML}")
  set_source_files_properties("${asm_object}" PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
  list(APPEND SNAPHAK_MASM_OBJECTS "${asm_object}")
endforeach()

add_library(snaphak_algo STATIC ${SNAPHAK_SOURCES} ${SNAPHAK_MASM_OBJECTS})
target_include_directories(snaphak_algo PRIVATE "${MH_SNAPHAK_DIR}")
target_compile_definitions(snaphak_algo PRIVATE SNAPHAKALGO_BUILDING NDEBUG _CONSOLE)
target_compile_options(snaphak_algo PRIVATE -O2 -fno-exceptions -fno-rtti -fms-compatibility -fms-extensions -msse4.1 -msse4.2 -mpopcnt -mlzcnt)
set_source_files_properties(
  "${MH_SNAPHAK_DIR}/impls/init_impls_avx2.cpp"
  PROPERTIES COMPILE_OPTIONS "-mavx;-mavx2;-mf16c;-mbmi;-mbmi2;-mfma")
set_source_files_properties(
  "${MH_SNAPHAK_DIR}/impls/init_impls_avx512.cpp"
  PROPERTIES COMPILE_OPTIONS "-mavx;-mavx2;-mavx512f;-mavx512cd;-mavx512bw;-mavx512dq;-mavx512vl")

add_library(udis86test STATIC ${UDIS_SOURCES})
target_include_directories(udis86test PRIVATE "${MH_UDIS_DIR}")
target_compile_definitions(udis86test PRIVATE _CRT_SECURE_NO_WARNINGS NDEBUG _CONSOLE)
target_compile_options(udis86test PRIVATE -O2 -fno-exceptions -fno-rtti -msse4.1 -msse4.2 -mpopcnt -mlzcnt)

add_library(m34thook SHARED ${MH_M34THOOK_SOURCES})
target_include_directories(m34thook PRIVATE
  "${MH_M34THOOK_DIR}"
  "${MH_SNAPHAK_DIR}"
  "${MH_RPC_DIR}"
  "${MH_UDIS_DIR}")
target_compile_definitions(m34thook PRIVATE MH_ETERNAL_V6 NDEBUG _CONSOLE)
target_compile_options(m34thook PRIVATE -O2 -fno-exceptions -fno-rtti -fms-compatibility -fms-extensions -msse4.1 -msse4.2 -flto=full)
target_link_options(m34thook PRIVATE -Xlinker /MACHINE:X64)
target_link_libraries(m34thook PRIVATE
  snaphak_algo
  udis86test
  rpcrt4
  version
  bcrypt
  kernel32
  user32
  gdi32
  winspool
  comdlg32
  advapi32
  shell32
  ole32
  oleaut32
  uuid
  odbc32
  odbccp32)
set_target_properties(m34thook PROPERTIES
  OUTPUT_NAME XINPUT1_3
  PREFIX ""
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
