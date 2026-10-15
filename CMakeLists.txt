cmake_minimum_required(VERSION 3.10)

project(RetroTransfer VERSION 1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

if(MINGW)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
endif()

include_directories(include)

set(RETRO_CORE_SOURCES
    src/scanner.cpp
    src/fingerprint.cpp
    src/huffman_codec.cpp
    src/compression_engine.cpp
    src/fixed_chunker.cpp
    src/cdc_chunker.cpp
    src/adaptive_policy.cpp
    src/chunk_engine.cpp
)

add_executable(RetroTransfer 
    src/main.cpp 
    ${RETRO_CORE_SOURCES}
)

add_executable(RetroTransferSetupCLI
    examples/transfer_setup_cli.cpp
)
add_dependencies(RetroTransferSetupCLI RetroTransfer)

enable_testing()

add_executable(test_chunking
    tests/test_chunking.cpp
    ${RETRO_CORE_SOURCES}
)

add_test(NAME ChunkingEngineTests COMMAND test_chunking)