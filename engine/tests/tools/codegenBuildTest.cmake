# Builds the codegenBuild fixture the ways a stale or needless codegen run
# shows: a target added after a first build, a deleted traits header, and an
# edited source that changes nothing. Run with cmake -P, given SOURCE (the
# fixture), WORK (a scratch directory), SFS_CODEGEN_CMAKE and GENERATOR.
file(REMOVE_RECURSE "${WORK}")
# A copy, so the edit below touches no file of the repo.
file(COPY "${SOURCE}/" DESTINATION "${WORK}/src")
set(SOURCE "${WORK}/src")
set(BUILD "${WORK}/build")

function(run what)
  execute_process(COMMAND ${ARGN} WORKING_DIRECTORY "${BUILD}"
                  RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "${what} failed:\n${out}\n${err}")
  endif()
  set(output "${out}${err}" PARENT_SCOPE)
endfunction()

function(configure)
  run(configure ${CMAKE_COMMAND} -S "${SOURCE}" -B "${BUILD}" -G "${GENERATOR}"
      "-DSFS_CODEGEN_CMAKE=${SFS_CODEGEN_CMAKE}" ${ARGN})
endfunction()

function(build)
  run(build ${CMAKE_COMMAND} --build "${BUILD}")
  string(REGEX MATCHALL "Generating[^\n]*" ran "${output}")
  list(LENGTH ran count)
  set(generated ${count} PARENT_SCOPE)
endfunction()

file(MAKE_DIRECTORY "${BUILD}")
configure(-DWITH_TESTS=OFF)
build()

# A target added after the first build gets the real header, not the stub
# written as it's configured (its build fails on the stub).
configure(-DWITH_TESTS=ON)
build()

# A deleted header is written again.
file(REMOVE "${BUILD}/generated/testsEventTraits.gen.h")
configure(-DWITH_TESTS=ON)
build()

# A deleted registration is written again after a reconfigure.
file(REMOVE "${BUILD}/generated/moduleReflection.gen.cpp")
configure(-DWITH_TESTS=ON)
build()

# An edit that changes no declaration runs codegen once, then never again.
file(TOUCH "${SOURCE}/module.cpp")
build()
if(NOT generated EQUAL 1)
  message(FATAL_ERROR "codegen ran ${generated} times after an edit")
endif()
build()
if(NOT generated EQUAL 0)
  message(FATAL_ERROR "codegen ran again (${generated}) with nothing changed")
endif()
