# Contributing

Build with a C++17 compiler on Linux or macOS. Run `make -j4`, `make test`, and `make integration` before opening a pull request. For memory ownership, parsing, or network changes, also run the sanitizer commands in the README.

Keep changes focused. Add regression coverage for behavior changes and update the command/protocol documentation when the public contract changes. Use descriptive names and small functions; do not add comments to code. Preserve binary-safe keys and values, and keep Make and CMake test lists aligned. Release builds must retain test assertions.

Pull requests should explain the problem, resulting behavior, validation performed, and any compatibility changes. Do not commit generated binaries, build directories, credentials, or local editor configuration.
