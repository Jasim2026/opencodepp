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
    agent_states_test)
  add_executable(${test} ${test}.cpp)
  target_link_libraries(${test} PRIVATE opencodepp_static)
  add_test(NAME ${test} COMMAND ${test})
endforeach()

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

# store_test runs the sqlite suite only when the optional backend is built.
target_compile_definitions(store_test PRIVATE
  OPENCODE_USE_SQLITE=$<BOOL:${OPENCODE_USE_SQLITE}>)
