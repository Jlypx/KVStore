option(KVSTORE_FETCH_GTEST "Fetch and build GoogleTest" OFF)

if(KVSTORE_FETCH_GTEST)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

add_executable(kvstore_smoke_test smoke_test.cpp)
target_link_libraries(kvstore_smoke_test PRIVATE kvstore_core)
kvstore_apply_compiler_warnings(kvstore_smoke_test)
kvstore_apply_sanitizers(kvstore_smoke_test)

add_test(NAME kvstore_smoke_test COMMAND kvstore_smoke_test)

add_executable(kvstore_wal_test wal_test.cpp)
target_link_libraries(kvstore_wal_test PRIVATE kvstore_core)
kvstore_apply_compiler_warnings(kvstore_wal_test)
kvstore_apply_sanitizers(kvstore_wal_test)
add_test(NAME kvstore_wal_test COMMAND kvstore_wal_test)

add_executable(kvstore_recovery_test recovery_test.cpp)
target_link_libraries(kvstore_recovery_test PRIVATE kvstore_core)
kvstore_apply_compiler_warnings(kvstore_recovery_test)
kvstore_apply_sanitizers(kvstore_recovery_test)
add_test(NAME kvstore_recovery_test COMMAND kvstore_recovery_test)

add_executable(kvstore_idempotency_test idempotency_test.cpp)
target_link_libraries(kvstore_idempotency_test PRIVATE kvstore_core)
kvstore_apply_compiler_warnings(kvstore_idempotency_test)
kvstore_apply_sanitizers(kvstore_idempotency_test)
add_test(NAME kvstore_idempotency_test COMMAND kvstore_idempotency_test)
