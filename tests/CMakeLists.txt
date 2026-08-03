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
    net_meter_offline_test)
  add_executable(${test} ${test}.cpp)
  target_link_libraries(${test} PRIVATE opencodepp_static)
  add_test(NAME ${test} COMMAND ${test})
endforeach()

# tokens_test loads its labeled corpus from the source tree.
set_tests_properties(tokens_test PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# config_test probes the environment (git detection) from inside the repo.
set_tests_properties(config_test PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")

# store_test runs the sqlite suite only when the optional backend is built.
target_compile_definitions(store_test PRIVATE
  OPENCODE_USE_SQLITE=$<BOOL:${OPENCODE_USE_SQLITE}>)
