# PATCH_COMMAND for the FetchContent dependencies (cmake/dependencies.cmake).
# Runs in the freshly extracted source tree:
#
#   cmake -DSRC=<source dir> -DARCHIVE=<downloaded archive> -DSTAMP=<hash>
#         [-DSTRIP=1] [-DPATCHES=a.patch,b.patch]
#         [-DCOPY_FROM=<dir> -DCOPY_TO=<subdir> -DCOPY_FILES=x.c,y.c]
#         [-DPOST=prog,arg,arg] -P apply_patches.cmake
#
# Patches are applied with `patch --fuzz=0`, so a patch that no longer matches
# fails the configure step instead of landing at the wrong place. STAMP is a
# hash of everything applied; it is written to <SRC>/.mt-patched. If the tree
# was already patched with a different set (a patch file changed, or a board
# selects another set), the tree is re-extracted from ARCHIVE first, so a
# patch is never applied on top of another.
cmake_minimum_required(VERSION 3.28)

foreach(v SRC ARCHIVE STAMP)
    if(NOT DEFINED ${v} OR "${${v}}" STREQUAL "")
        message(FATAL_ERROR "apply_patches.cmake: ${v} not set")
    endif()
endforeach()
if(NOT DEFINED STRIP)
    set(STRIP 1)
endif()
foreach(v PATCHES COPY_FILES POST)
    if(DEFINED ${v})
        string(REPLACE "," ";" ${v} "${${v}}")
    endif()
endforeach()

set(marker "${SRC}/.mt-patched")
if(EXISTS "${marker}")
    file(READ "${marker}" old)
    string(STRIP "${old}" old)
    if(old STREQUAL STAMP)
        message(STATUS "${SRC}: patches already applied")
        return()
    endif()
    message(STATUS "${SRC}: patched with a different set, re-extracting ${ARCHIVE}")
    get_filename_component(parent "${SRC}" DIRECTORY)
    set(tmp "${parent}/.mt-extract")
    file(REMOVE_RECURSE "${tmp}")
    file(ARCHIVE_EXTRACT INPUT "${ARCHIVE}" DESTINATION "${tmp}")
    file(GLOB top LIST_DIRECTORIES true "${tmp}/*")
    list(LENGTH top n)
    if(NOT n EQUAL 1)
        message(FATAL_ERROR "${ARCHIVE}: expected one top-level directory")
    endif()
    file(GLOB old_entries LIST_DIRECTORIES true "${SRC}/*" "${SRC}/.*")
    if(old_entries)
        file(REMOVE_RECURSE ${old_entries})
    endif()
    file(GLOB new_entries LIST_DIRECTORIES true "${top}/*" "${top}/.*")
    foreach(e IN LISTS new_entries)
        get_filename_component(name "${e}" NAME)
        file(RENAME "${e}" "${SRC}/${name}")
    endforeach()
    file(REMOVE_RECURSE "${tmp}")
endif()

foreach(p IN LISTS PATCHES)
    message(STATUS "Applying ${p}")
    execute_process(
        COMMAND patch -p${STRIP} --forward --batch --fuzz=0 --no-backup-if-mismatch -i "${p}"
        WORKING_DIRECTORY "${SRC}"
        RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${p} does not apply to ${SRC} (patch exit ${rc}); "
                            "the dependency pin and the patch are out of step")
    endif()
endforeach()

foreach(f IN LISTS COPY_FILES)
    if(NOT EXISTS "${SRC}/${COPY_TO}/${f}")
        message(FATAL_ERROR "${COPY_TO}/${f} missing from ${SRC}; refusing to add a file upstream does not have")
    endif()
    file(COPY_FILE "${COPY_FROM}/${f}" "${SRC}/${COPY_TO}/${f}")
endforeach()

if(POST)
    execute_process(COMMAND ${POST} WORKING_DIRECTORY "${SRC}" RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${SRC}: '${POST}' failed (${rc})")
    endif()
endif()

file(WRITE "${marker}" "${STAMP}\n")
