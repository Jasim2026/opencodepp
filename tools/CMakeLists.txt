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
