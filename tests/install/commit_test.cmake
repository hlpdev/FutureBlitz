# fb --version's commit is a checkout's own: a git repository's top folder gets its commit, and a
# plain folder inside it, as a source archive unpacked there, gets "unknown".

include("${FB_SOURCE}/cmake/commit.cmake")
if(NOT GIT_FOUND)
  message(STATUS "git isn't here, so there's nothing to check")
  return()
endif()

set(outer "${FB_OUTPUT}/outer")
file(REMOVE_RECURSE "${FB_OUTPUT}")
file(MAKE_DIRECTORY "${outer}/FutureBlitz-0.1.0")
file(WRITE "${outer}/readme.txt" "outer\n")
foreach(step IN ITEMS "init;-q" "add;readme.txt"
             "-c;user.name=test;-c;user.email=test@example.com;commit;-q;-m;outer")
  execute_process(COMMAND "${GIT_EXECUTABLE}" ${step} WORKING_DIRECTORY "${outer}"
    OUTPUT_QUIET COMMAND_ERROR_IS_FATAL ANY)
endforeach()
execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD WORKING_DIRECTORY "${outer}"
  OUTPUT_VARIABLE expected OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)

fb_source_commit("${outer}" top)
fb_source_commit("${outer}/FutureBlitz-0.1.0" inside)
if(NOT top STREQUAL expected OR NOT inside STREQUAL "unknown")
  message(FATAL_ERROR "the checkout's commit is '${top}', not '${expected}', and the folder "
    "inside it has '${inside}', not 'unknown'")
endif()
