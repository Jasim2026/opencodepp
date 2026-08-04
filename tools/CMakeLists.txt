# tools/ -- developer + QA tools. Always buildable with stdlib only.

add_executable(bench_engine bench_main.cpp)
target_link_libraries(bench_engine PRIVATE opencodepp_static)

add_executable(mock_api mock_api.cpp)
target_link_libraries(mock_api PRIVATE opencodepp_static)

add_executable(drill drill.cpp)
target_link_libraries(drill PRIVATE opencodepp_static)
add_test(NAME drill COMMAND drill)

add_executable(probe probe.cpp)
target_link_libraries(probe PRIVATE opencodepp_static)
add_test(NAME probe_mock COMMAND probe --mock)

add_executable(bench_prompt bench_prompt.cpp)
target_link_libraries(bench_prompt PRIVATE opencodepp_static)

add_executable(bench_graph bench_graph.cpp)
target_link_libraries(bench_graph PRIVATE opencodepp_static)

add_executable(run_agent run_agent.cpp)
target_link_libraries(run_agent PRIVATE opencodepp_static)
add_test(NAME run_agent_mock COMMAND run_agent --mock)
add_test(NAME run_agent_fault COMMAND run_agent --mock --fault 2)

# tools/soak -- long-run resilience driver. The ctest entry is a short soak;
# the dedicated CI job runs the longer one.
add_executable(soak soak.cpp)
add_test(NAME soak_short COMMAND soak --bin $<TARGET_FILE:run_agent>
  --tasks 3 --fault-every 2 --task-timeout 30)

set_tests_properties(run_agent_mock run_agent_fault soak_short PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_executable(opencode_cli opencode_cli.cpp)
target_link_libraries(opencode_cli PRIVATE opencodepp_static)
set_target_properties(opencode_cli PROPERTIES OUTPUT_NAME "opencodepp_cli")

# The reference CLI ships in the install tree (bin/opencodepp_cli).
install(TARGETS opencode_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# tools/measure -- Phase 13 T2 measurement + hardening gate. The binary-size
# budget applies only to the stripped -Os build; other presets are
# informational (dev builds carry ASan/debug overhead by design).
add_executable(measure measure.cpp)
target_link_libraries(measure PRIVATE opencodepp_static)
if(CMAKE_CXX_FLAGS MATCHES "-Os")
  set(OPENCODE_SIZE_LIMIT_MB 15)
else()
  set(OPENCODE_SIZE_LIMIT_MB 0)
endif()
add_test(NAME hardening
  COMMAND measure --bin $<TARGET_FILE:opencode_cli> --trials 3
          --size-limit ${OPENCODE_SIZE_LIMIT_MB})
set_tests_properties(hardening PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")
