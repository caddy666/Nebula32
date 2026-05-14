mkdir build && cd build
cmake -DBUILD_TESTS=ON ..
make
ctest  # Runs all tests and reports pass/fail
