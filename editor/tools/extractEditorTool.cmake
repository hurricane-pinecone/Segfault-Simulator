# Extracts one of the Code panel's tools (its release archive) into DEST, replacing what was there. An archive that
# holds a single top-level folder (as release archives do) has that folder's contents become DEST's. LICENSE, when
# given, is copied in beside them. Run by the SfsEditorTools target (editorTools.cmake):
#
#   cmake -DARCHIVE=<archive> -DDEST=<folder> [-DLICENSE=<file>] -P extractEditorTool.cmake
set(staging "${DEST}.extracting")
file(REMOVE_RECURSE "${DEST}" "${staging}")
file(ARCHIVE_EXTRACT INPUT "${ARCHIVE}" DESTINATION "${staging}")

file(GLOB entries LIST_DIRECTORIES true "${staging}/*")
list(LENGTH entries count)
if(count EQUAL 1 AND IS_DIRECTORY "${entries}")
    file(RENAME "${entries}" "${DEST}")
    file(REMOVE_RECURSE "${staging}")
else()
    file(RENAME "${staging}" "${DEST}")
endif()

if(LICENSE)
    file(COPY "${LICENSE}" DESTINATION "${DEST}")
endif()
