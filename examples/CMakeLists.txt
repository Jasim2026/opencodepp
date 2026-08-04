# examples/ -- working hosts against the frozen C ABI.

add_executable(embed_cli embed_cli.cpp)
target_link_libraries(embed_cli PRIVATE opencodepp_static)

add_executable(event_host event_host.cpp)
target_link_libraries(event_host PRIVATE opencodepp_static)
