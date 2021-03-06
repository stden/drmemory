# **********************************************************
# Copyright (c) 2010-2012 Google, Inc.    All rights reserved.
# Copyright (c) 2010 VMware, Inc.    All rights reserved.
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

cmake_minimum_required(VERSION 2.6)

# symbol access library

if (X64)
  set(BITS "64")
else ()
  set(BITS "32")
endif ()

# There's no simple automated solution to get clients using a static drsyms
# to omit /noentry and link with libcmt.lib prior to linking w/ DR,
# unless we change configure_DynamoRIO_client() to take in a list of all
# extensions that will be used.
# Since a static lib is non-default it seems reasonable to require the
# developer to set DynamoRIO_USE_LIBC.
option(DR_EXT_DRSYMS_STATIC "create drsyms as a static, not shared, library (N.B.: because drsyms uses libc, any client using drsyms as a static library must set DynamoRIO_USE_LIBC before calling configure_DynamoRIO_client()")
if (DR_EXT_DRSYMS_STATIC)
  set(libtype STATIC)
else()
  set(libtype SHARED)
endif ()

# we need libc b/c our elftoolchain libraries use it
set(DynamoRIO_USE_LIBC ON)

if (WIN32)
  set(srcs
    drsyms_windows.c drsyms_unix.c drsyms_pecoff.c
    drsyms_dwarf.c demangle.cc drsyms_common.c)
  set(dwarf_dir "${PROJECT_SOURCE_DIR}/ext/drsyms/libelftc-pecoff/lib${BITS}")
  set(dwarf_libpath "${dwarf_dir}/dwarf.lib")
  set(elftc_libpath "${dwarf_dir}/elftc.lib")
  configure_file("${dwarf_dir}/dwarf.pdb"
    "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/dwarf.pdb" COPYONLY)
  configure_file("${dwarf_dir}/elftc.pdb"
    "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/elftc.pdb" COPYONLY)
  # For DR_EXT_DRSYMS_STATIC:
  # Listing the dependent libs as sources does get them combined into
  # drsyms static lib for pre-VS2010, but VS2010 won't do that, so we go
  # with exported separate libs to match Linux (alternative would be a custom
  # command to combine via lib.exe).
  add_library(drsyms ${libtype} ${srcs})
  # while private loader means preferred base is not required, more efficient
  # to avoid rebase so we avoid conflict w/ client and other exts
  set(PREFERRED_BASE 0x76000000)
  configure_DynamoRIO_client(drsyms)
  target_link_libraries(drsyms dbghelp)
elseif (UNIX)
  add_library(drsyms ${libtype}
    drsyms_linux.c drsyms_unix.c drsyms_elf.c
    drsyms_dwarf.c demangle.cc drsyms_common.c)
  configure_DynamoRIO_client(drsyms)
  set(dwarf_libpath "${PROJECT_SOURCE_DIR}/ext/drsyms/libelftc/lib${BITS}/libdwarf.a")
  set(elftc_libpath "${PROJECT_SOURCE_DIR}/ext/drsyms/libelftc/lib${BITS}/libelftc.a")
  set(elf_libpath "${PROJECT_SOURCE_DIR}/ext/drsyms/libelftc/lib${BITS}/libelf.a")
endif (WIN32)

if (DR_EXT_DRSYMS_STATIC)
  # if drsyms is built static we need to include these with an exports path
  # in DynamoRIOTarget*.cmake and not with the source path here:
  add_library(dwarf STATIC IMPORTED)
  set_property(TARGET dwarf PROPERTY IMPORTED_LOCATION "${dwarf_libpath}")
  add_library(elftc STATIC IMPORTED)
  set_property(TARGET elftc PROPERTY IMPORTED_LOCATION "${elftc_libpath}")
  target_link_libraries(drsyms dwarf elftc)
  if (UNIX)
    add_library(elf STATIC IMPORTED)
    set_property(TARGET elf PROPERTY IMPORTED_LOCATION "${elf_libpath}")
    target_link_libraries(drsyms elf)
  endif (UNIX)
else (DR_EXT_DRSYMS_STATIC)
  target_link_libraries(drsyms "${dwarf_libpath}" "${elftc_libpath}")
  if (UNIX)
    target_link_libraries(drsyms "${elf_libpath}")
  endif (UNIX)
  # i#693: CMake will try to export the path to the static libs we use via
  # IMPORTED_LINK_INTERFACE_LIBRARIES_NOCONFIG, but they won't exist on the
  # user's machine.  Clearing this property prevents that.
  set_target_properties(drsyms PROPERTIES LINK_INTERFACE_LIBRARIES "")
endif (DR_EXT_DRSYMS_STATIC)

include_directories("${PROJECT_SOURCE_DIR}/ext/drsyms/libelftc/include")

use_DynamoRIO_extension(drsyms drcontainers)
if (UNIX)
  # static libs must be PIC to be linked into clients: else requires
  # relocations that run afoul of security policies, etc.
  set(extra_flags "-fPIC")
  # avoid missing symbols in DR_EXT_DRSYMS_STATIC config from g++ libs when
  # drsyms_bench is linked with gcc instead of g++ (i#715, happens w/ cmake
  # < 2.8.0 where demangle.cc is not propagated through libdrsyms.a)
  set(extra_flags "${extra_flags} -fno-exceptions")
  append_property_string(TARGET drsyms COMPILE_FLAGS "${extra_flags}")
endif (UNIX)

# ensure we rebuild if includes change
add_dependencies(drsyms api_headers)

add_executable(drsyms_bench drsyms_bench.c)
configure_DynamoRIO_standalone(drsyms_bench)
use_DynamoRIO_extension(drsyms_bench drsyms)
# we don't want drsyms_bench installed so we avoid the standard location
set_target_properties(drsyms_bench PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY${location_suffix} "${PROJECT_BINARY_DIR}/ext")

if (WIN32 AND GENERATE_PDBS)
  # I believe it's the lack of CMAKE_BUILD_TYPE that's eliminating this?
  # In any case we make sure to add it (for release and debug, to get pdb):
  append_property_string(TARGET drsyms LINK_FLAGS "/debug")
endif ()

if (DR_EXT_DRSYMS_STATIC)
  if (WIN32)
    get_target_property(cur_flags drsyms COMPILE_FLAGS)
    set_target_properties(drsyms PROPERTIES COMPILE_FLAGS "${cur_flags} /DSTATIC_LIB")
  endif (WIN32)
  DR_install(FILES "${dwarf_libpath}" "${elf_libpath}" "${elftc_libpath}"
    DESTINATION ${INSTALL_EXT_LIB})
  if (WIN32)
   DR_install(FILES "${dwarf_dir}/dwarf.pdb" "${dwarf_dir}/elftc.pdb"
      DESTINATION ${INSTALL_EXT_LIB}
      PERMISSIONS OWNER_READ OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
      WORLD_READ WORLD_EXECUTE)
  endif (WIN32)
endif (DR_EXT_DRSYMS_STATIC)

# documentation is put into main DR docs/ dir
export(TARGETS drsyms FILE ${PROJECT_BINARY_DIR}/cmake/${exported_targets_name}.cmake
  APPEND)
DR_install(TARGETS drsyms EXPORT ${exported_targets_name} DESTINATION ${INSTALL_EXT_LIB})
DR_install(FILES drsyms.h DESTINATION ${INSTALL_EXT_INCLUDE})
