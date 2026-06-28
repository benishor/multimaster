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

# Crypto unit tests only build when the library has libsodium support.
if(MULTIMASTER_HAVE_CRYPTO)
    add_executable(crypto_test crypto_test.cpp)
    target_link_libraries(crypto_test PRIVATE multimaster::multimaster)
    target_include_directories(crypto_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME crypto_test COMMAND crypto_test)

    add_executable(secure_smoke_test secure_smoke_test.cpp)
    target_link_libraries(secure_smoke_test PRIVATE multimaster::multimaster)
    target_include_directories(secure_smoke_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME secure_smoke_test COMMAND secure_smoke_test)
    set_tests_properties(secure_smoke_test PROPERTIES TIMEOUT 60)

    add_executable(identity_test identity_test.cpp)
    target_link_libraries(identity_test PRIVATE multimaster::multimaster)
    target_include_directories(identity_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME identity_test COMMAND identity_test)
    set_tests_properties(identity_test PROPERTIES TIMEOUT 60)
endif()
