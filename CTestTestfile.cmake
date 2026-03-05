# Compatibility shim for CTest versions that ignore --test-dir.
# CTest 3.16 in this environment executes tests from source root.
add_test(kvstore_smoke_test "build/tests/kvstore_smoke_test")
