# Runs titan-engine-host and titan-protocol-check concurrently (execute_process runs piped
# commands in parallel). The checker ends with engine.shutdown so the host exits.
if(NOT HOST OR NOT CHECK OR NOT WORK)
  message(FATAL_ERROR "HOST, CHECK and WORK are required")
endif()
string(RANDOM LENGTH 8 _tag)
set(_sock "/tmp/titan-conf-${_tag}.sock")
execute_process(
  COMMAND ${HOST} --socket ${_sock}
  COMMAND ${CHECK} --socket ${_sock} --workdir ${WORK} --shutdown --report ${WORK}-report.json
  RESULTS_VARIABLE _results
  TIMEOUT 60)
message(STATUS "host/check exit codes: ${_results}")
list(GET _results 1 _check_rc)
if(NOT _check_rc EQUAL 0)
  message(FATAL_ERROR "protocol conformance failed (${_check_rc})")
endif()
