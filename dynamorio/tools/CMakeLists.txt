# **********************************************************
# Copyright (c) 2010-2012 Google, Inc.    All rights reserved.
# Copyright (c) 2009-2010 VMware, Inc.    All rights reserved.
# **********************************************************

# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
# 
# * Redistributions of source code must retain the above copyright notice,
#   this list of conditions and the following disclaimer.
# 
# * Redistributions in binary form must reproduce the above copyright notice,
#   this list of conditions and the following disclaimer in the documentation
#   and/or other materials provided with the distribution.
# 
# * Neither the name of VMware, Inc. nor the names of its contributors may be
#   used to endorse or promote products derived from this software without
#   specific prior written permission.
# 
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL VMWARE, INC. OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
# SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
# CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
# LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
# OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
# DAMAGE.

add_executable(run_in_bg run_in_bg.c)

if (UNIX)
  add_executable(runstats runstats.c)
  add_executable(nudgeunix nudgeunix.c ${PROJECT_SOURCE_DIR}/core/linux/nudgesig.c)
  add_executable(drloader drloader.c)

  include_directories(
   ${PROJECT_SOURCE_DIR}/core
   ${PROJECT_SOURCE_DIR}/core/lib
  )

  add_definitions(-DNOT_DYNAMORIO_CORE)

  # we generate 3 different scripts from drdeploy.in
  if (X64)
    set(archdefault 64)
    set(archother 32)
  else (X64)
    set(archdefault 32)
    set(archother 64)
  endif (X64)
  set(config 1)
  set(runapp 0)
  set(name "drconfig")
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/drdeploy.in"
    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${name}" @ONLY)
  set(config 1)
  set(runapp 1)
  set(name "drrun")
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/drdeploy.in"
    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${name}" @ONLY)
  set(config 0)
  set(runapp 1)
  set(name "drinject")
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/drdeploy.in"
    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${name}" @ONLY)
  # we leave drdeploy for backward compat w/ old script
  execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink
    "drrun" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/drdeploy"
    RESULT_VARIABLE symlink_result
    ERROR_VARIABLE symlink_error
    OUTPUT_QUIET)
  if (symlink_result OR symlink_error)
    message(FATAL_ERROR "*** drdeploy symlink failed: ***\n${symlink_error}")
  endif (symlink_result OR symlink_error)
  # simpler to install all and exclude
  DR_install(DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/"
    DESTINATION "${INSTALL_BIN}"
    FILE_PERMISSIONS OWNER_READ OWNER_EXECUTE 
    GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
    PATTERN "run_in_bg*"
    PATTERN "runstats*"
    PATTERN "*_exe"  # Don't install the native drdeploy variants for now.
    EXCLUDE)

  # Set up our debugging support for gdb in the build directory.
  # We need to compute the final path of libdynamorio.so, with version number
  # applied for gdb's script autoloading to work.
  get_target_property(libdynamorio_so dynamorio LOCATION)
  get_target_property(soversion dynamorio SOVERSION)
  set(libdynamorio_so "${libdynamorio_so}.${soversion}")
  execute_process(COMMAND
    ${CMAKE_COMMAND} -E create_symlink
    "${CMAKE_CURRENT_SOURCE_DIR}/libdynamorio.so-gdb.py"
    "${libdynamorio_so}-gdb.py"
    WORKING_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"
    OUTPUT_QUIET)
  configure_file(gdbinit ${CMAKE_BINARY_DIR}/.gdbinit)

  # Drops the last path element from path and stores it in path_out.
  function (dirname path_out path)
    string(REGEX REPLACE "/[^/]*$" "" path "${path}")
    set(${path_out} "${path}" PARENT_SCOPE)
  endfunction (dirname)

  # Links a target against a list of libraries, and adds relative rpaths
  # pointing to the directories of the libraries.
  #
  # By default, CMake sets an absolute rpath to the build directory, which it
  # strips at install time.  By adding our own relative rpath, so long as the
  # target and its libraries stay in the same layout relative to each other,
  # the loader will be able to find the libraries.  We assume that the layout
  # is the same in the build and install directories.
  function (link_libs_with_rel_rpath target)
    target_link_libraries(${target} ${ARGN})

    # Turn off the default CMake rpath setting and add our own LINK_FLAGS.
    set_target_properties(${target} PROPERTIES SKIP_BUILD_RPATH ON)
    foreach (lib ${ARGN})
      # Compute the relative path between the directory of the target and the
      # library it is linked against.
      get_target_property(tgt_path ${target} LOCATION)
      get_target_property(lib_path ${lib} LOCATION)
      dirname(tgt_path "${tgt_path}")
      dirname(lib_path "${lib_path}")
      file(RELATIVE_PATH relpath "${tgt_path}" "${lib_path}")

      # Append the new rpath element if it isn't there already.
      set(new_lflag "-Wl,-rpath='$$ORIGIN/${relpath}'")
      get_target_property(lflags ${target} LINK_FLAGS)
      if (NOT lflags MATCHES new_lflag)
        append_property_string(TARGET ${target} LINK_FLAGS "${new_lflag}")
      endif ()
    endforeach ()
  endfunction (link_libs_with_rel_rpath)

  # FIXME i#840: Drop the _exe suffix, replace the shell scripts, and combine
  # with the Windows drdeploy.c targets below.
  # Add a native version of drconfig.
  add_executable(drconfig_exe drdeploy.c)
  link_libs_with_rel_rpath(drconfig_exe drconfiglib)
  set_target_properties(drconfig_exe PROPERTIES COMPILE_FLAGS "-DDRCONFIG")
  # Add a native version of drrun.
  add_executable(drrun_exe drdeploy.c)
  link_libs_with_rel_rpath(drrun_exe drconfiglib drinjectlib)
  set_target_properties(drrun_exe PROPERTIES COMPILE_FLAGS "-DDRRUN")

  DR_install(TARGETS nudgeunix DESTINATION ${INSTALL_BIN})
else (UNIX)
  # FIXME i#98: eventually upgrade to W4 with pragma exceptions.
  # W2 is default (we're using W3).  We should also replace
  # all references to unsafe functions (e.g., fopen) and
  # remove /wd4996
  string(REGEX REPLACE "/W4" "/W3" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
  set(tool_cflags "/wd4996")
  add_definitions(-DNOT_DYNAMORIO_CORE -DWIN32)

  set(RESOURCES ${PROJECT_SOURCE_DIR}/core/win32/resources.rc)

  set(tgts 
    DRview 
    DRcontrol 
    DRkill 
    closewnd
    winstats 
    svccntrl 
    DRload
    balloon 
    dummy 
    create_process 
    )
  if (NOT X64)
    # FIXME i#118: ldmp not yet 64-bit
    set(tgts ${tgts} ldmp)
  endif (NOT X64)

  include_directories(
    ${PROJECT_SOURCE_DIR}/libutil
    ${PROJECT_SOURCE_DIR}/core
    ${PROJECT_SOURCE_DIR}/core/lib
    )

  target_link_libraries(run_in_bg ntdll)

  foreach (tgt ${tgts})
    add_executable(${tgt} ${tgt}.c ${RESOURCES})

    if ("${tgt}" STREQUAL "ldmp")
      set(extra_lib "ntdll")
    endif ("${tgt}" STREQUAL "ldmp")
    target_link_libraries(${tgt}
      policy_static user32 advapi32 pdh imagehlp shell32 "${extra_lib}")

    string(REGEX MATCH "(closewnd|winstats|create_process)" no_unicode "${tgt}")
    if (no_unicode)
      set(unicode_cflags "")
    else (no_unicode)
      set(unicode_cflags "-DUNICODE -D_UNICODE")
    endif (no_unicode)

    set_target_properties(${tgt} PROPERTIES
      # Set define parameters for resources.rc
      COMPILE_FLAGS "${tool_cflags} ${unicode_cflags} -DRC_IS_${tgt}")
  endforeach (tgt)

  # we generate 3 different tools from drdeploy.c
  add_executable(drconfig drdeploy.c ${RESOURCES})
  set_target_properties(drconfig PROPERTIES
    COMPILE_FLAGS "${tool_cflags} -DRC_IS_drconfig -DDRCONFIG")
  add_executable(drrun drdeploy.c ${RESOURCES})
  set_target_properties(drrun PROPERTIES
    COMPILE_FLAGS "${tool_cflags} -DRC_IS_drrun -DDRRUN")
  add_executable(drinject drdeploy.c ${RESOURCES})
  set_target_properties(drinject PROPERTIES
    COMPILE_FLAGS "${tool_cflags} -DRC_IS_drinject -DDRINJECT")
  set(tgts ${tgts} drconfig drrun drinject)
  foreach (deploytgt drconfig drrun drinject)
    target_link_libraries(${deploytgt}
      policy_static user32 advapi32 pdh imagehlp shell32 drconfiglib drinjectlib)
  endforeach ()

  # drdeploy.exe links to libutil/drconfiglib.dll.  When we build drdeploy,
  # we'll copy the DLL to the tools directory so users won't need to
  # update their path.

  # We could add a manifest so that we can request elevation on Vista
  # (xref PR 233520).  Works fine for double clicking the app, but
  # running from a cmd shell just gives access denied if the cmd shell
  # isn't runas admin which isn't quite as nice.  Disable for now.
  # $(MT) -manifest drdeploy.exe.manifest -outputresource:drdeploy.exe

  DR_install(TARGETS ${tgts} DESTINATION "${INSTALL_BIN}")

endif (UNIX)

if (X64)
  set(SYMSFX "64")
else (X64)
  set(SYMSFX "")
endif (X64)

DR_install(FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/windbg-scripts/load_syms${SYMSFX}"
  DESTINATION "${INSTALL_BIN}"
  RENAME "load_syms${SYMSFX}.txt")
