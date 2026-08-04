# tests/ -- one test executable per module, wired into CTest.
# Tests live at tests/<module>_test.cpp co-located with their module (03_ARCHITECTURE §2).

add_executable(smoke_test smoke_test.cpp)
target_link_libraries(smoke_test PRIVATE opencodepp_static)
add_test(NAME smoke_test COMMAND smoke_test)

foreach(test
    arena_test
    log_test
    metrics_test
    event_loop_test
    channel_test
    task_test
    util_test
    msg_test
    codec_test
    tokens_test
    config_test
    model_test
    store_test
    net_socket_test
    net_http_test
    net_pool_test
    net_meter_offline_test
    provider_test
    prompt_test
    graph_test
    tools_test
    permission_test
    shell_test
    verify_test
    verify_golden_test
    agent_states_test
    agent_loop_test
    memory_test
    fuzz_test)
  add_executable(${test} ${test}.cpp)
  target_link_libraries(${test} PRIVATE opencodepp_static)
  add_test(NAME ${test} COMMAND ${test})
endforeach()

# A second fuzz seed for diversity; the dev preset (ASan/UBSan) is the gate.
add_test(NAME fuzz_test_extra COMMAND fuzz_test --seed 7 --iters 300)

# provider_test reads its golden wire fixtures from the source tree.
set_tests_properties(provider_test PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# tokens_test loads its labeled corpus from the source tree.
set_tests_properties(tokens_test PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# config_test probes the environment (git detection) from inside the repo.
set_tests_properties(config_test PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")

# prompt_test loads src/prompt/templates and the provider wire fixtures, so it
# runs from the repo root.
set_tests_properties(prompt_test PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")

# graph_test indexes the workspace fixtures, so it runs from the repo root.
set_tests_properties(graph_test PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")

# tools_test reads golden wire fixtures and builds its sandbox workspace from
# the repo root.
set_tests_properties(tools_test PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")

# abi_test.c is a pure C11 program that links only the C ABI (opencodepp_shared)
# and needs the prompt templates + workspace from the repo root.
add_executable(abi_test abi_test.c)
target_link_libraries(abi_test PRIVATE opencodepp_shared)
add_test(NAME abi_test COMMAND abi_test)
set_tests_properties(abi_test PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")

# cli_e2e_test drives the real opencode_cli binary as a subprocess against an
# in-test multi-round SSE mock; needs prompt templates + workspace at repo root.
add_executable(cli_e2e_test cli_e2e_test.cpp)
target_link_libraries(cli_e2e_test PRIVATE opencodepp_static)
add_test(NAME cli_e2e_test COMMAND cli_e2e_test $<TARGET_FILE:opencode_cli>)
set_tests_properties(cli_e2e_test PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")

# permission_test runs from the repo root (same sandbox convention as tools_test).
set_tests_properties(permission_test PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")

# shell_test runs from the repo root.
set_tests_properties(shell_test PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")

# verify_test runs from the repo root (reads golden fixtures in later commits).
set_tests_properties(verify_test PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")

# verify_golden_test runs from the repo root.
set_tests_properties(verify_golden_test PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")

# agent tests run from the repo root.
set_tests_properties(agent_states_test PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(agent_loop_test PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")

# store_test runs the sqlite suite only when the optional backend is built.
target_compile_definitions(store_test PRIVATE
  OPENCODE_USE_SQLITE=$<BOOL:${OPENCODE_USE_SQLITE}>)
