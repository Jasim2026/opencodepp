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
    util_test)
  add_executable(${test} ${test}.cpp)
  target_link_libraries(${test} PRIVATE opencodepp_static)
  add_test(NAME ${test} COMMAND ${test})
endforeach()
