# tools/ — developer + QA tools. Always buildable with stdlib only.

add_executable(bench_engine bench_main.cpp)
target_link_libraries(bench_engine PRIVATE opencodepp_static)

add_executable(mock_api mock_api.cpp)
target_link_libraries(mock_api PRIVATE opencodepp_static)
