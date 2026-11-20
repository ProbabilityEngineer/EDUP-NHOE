# Writes edup_version.h with `git describe --tags --match v* --always --dirty`
# for the tree as it stands at BUILD time (not configure time), so a DLL always names
# the exact source it came from; uncommitted changes show as -dirty. The header
# is rewritten only when the text changes, so unchanged builds stay no-ops.
# Only release tags (v*) count, so archive/* tags never become the version.
# No git or no repository falls back to "unknown": versioning never fails a build.
#   cmake -DSRC=<source dir> -DOUT=<header path> -P cmake/version.cmake
set(describe "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SRC}" describe --tags --match "v*" --always --dirty
                  OUTPUT_VARIABLE out RESULT_VARIABLE rc OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(rc EQUAL 0 AND out)
    set(describe "${out}")
  endif()
endif()
set(text "#pragma once\n#define EDUP_BUILD_DESCRIBE \"${describe}\"\n")
set(current "")
if(EXISTS "${OUT}")
  file(READ "${OUT}" current)
endif()
if(NOT current STREQUAL text)
  file(WRITE "${OUT}" "${text}")
endif()
