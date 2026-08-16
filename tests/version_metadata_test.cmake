# nasmount release-version consistency gate.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The source VERSION file is intentionally easy for release automation to
# consume. This gate proves that the two generated product surfaces still
# carry the exact value CMake configured as PROJECT_VERSION.

foreach(required VERSION_FILE GENERATED_HEADER KCM_METADATA EXPECTED_VERSION)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "version metadata test requires ${required}")
    endif()
endforeach()

file(READ "${VERSION_FILE}" source_version)
string(STRIP "${source_version}" source_version)
if(NOT source_version STREQUAL EXPECTED_VERSION)
    message(FATAL_ERROR
        "VERSION contains '${source_version}', but CMake configured "
        "PROJECT_VERSION as '${EXPECTED_VERSION}'")
endif()

file(READ "${GENERATED_HEADER}" generated_header)
string(FIND "${generated_header}" "Version[] = \"${EXPECTED_VERSION}\"" header_match)
if(header_match EQUAL -1)
    message(FATAL_ERROR
        "generated C++ version header does not contain '${EXPECTED_VERSION}'")
endif()

file(READ "${KCM_METADATA}" kcm_metadata)
string(JSON kcm_version ERROR_VARIABLE json_error
       GET "${kcm_metadata}" KPlugin Version)
if(NOT json_error STREQUAL "NOTFOUND")
    message(FATAL_ERROR "cannot read KCM version metadata: ${json_error}")
endif()
if(NOT kcm_version STREQUAL EXPECTED_VERSION)
    message(FATAL_ERROR
        "KCM metadata contains '${kcm_version}', expected '${EXPECTED_VERSION}'")
endif()

message(STATUS "All generated version metadata agrees on ${EXPECTED_VERSION}")
