# tests/ — one test executable per module, wired into CTest.
# Tests live at tests/<module>_test.cpp co-located with their module (03_ARCHITECTURE §2).

add_executable(smoke_test smoke_test.cpp)
target_link_libraries(smoke_test PRIVATE opencodepp_static)
add_test(NAME smoke_test COMMAND smoke_test)
