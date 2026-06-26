# Dependency-free test harness: one executable per test file, registered with
# CTest. Tests may include private headers from src/.

add_executable(wire_test wire_test.cpp)
target_link_libraries(wire_test PRIVATE multimaster::multimaster)
target_include_directories(wire_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME wire_test COMMAND wire_test)

add_executable(dedup_test dedup_test.cpp)
target_link_libraries(dedup_test PRIVATE multimaster::multimaster)
target_include_directories(dedup_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME dedup_test COMMAND dedup_test)

add_executable(smoke_test smoke_test.cpp)
target_link_libraries(smoke_test PRIVATE multimaster::multimaster)
target_include_directories(smoke_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME smoke_test COMMAND smoke_test)
set_tests_properties(smoke_test PROPERTIES TIMEOUT 60)
