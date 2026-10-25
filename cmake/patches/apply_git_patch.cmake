# Applies a git patch to a FetchContent source tree, idempotently.
# Usage (as PATCH_COMMAND, run with WORKING_DIRECTORY = fetched source dir):
#   ${CMAKE_COMMAND} -DPATCH_FILE=<abs path> -P <this file>
# Behaviour:
#   - patch already applied (reverse check passes): skip, no error
#   - patch applies cleanly: apply it
#   - neither: FATAL_ERROR, so configure stops loudly
if(NOT PATCH_FILE OR NOT EXISTS "${PATCH_FILE}")
    message(FATAL_ERROR "apply_git_patch: PATCH_FILE missing: '${PATCH_FILE}'")
endif()
find_package(Git QUIET)
if(NOT GIT_EXECUTABLE)
    set(GIT_EXECUTABLE git)
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" apply --reverse --check --ignore-whitespace "${PATCH_FILE}"
    RESULT_VARIABLE _rev OUTPUT_QUIET ERROR_QUIET)
if(_rev EQUAL 0)
    message(STATUS "apply_git_patch: already applied, skipping: ${PATCH_FILE}")
    return()
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" apply --check --ignore-whitespace "${PATCH_FILE}"
    RESULT_VARIABLE _chk ERROR_VARIABLE _chk_err)
if(NOT _chk EQUAL 0)
    message(FATAL_ERROR "apply_git_patch: patch does NOT apply: ${PATCH_FILE}\n${_chk_err}")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace "${PATCH_FILE}"
    RESULT_VARIABLE _app ERROR_VARIABLE _app_err)
if(NOT _app EQUAL 0)
    message(FATAL_ERROR "apply_git_patch: git apply failed: ${PATCH_FILE}\n${_app_err}")
endif()
message(STATUS "apply_git_patch: applied ${PATCH_FILE}")
