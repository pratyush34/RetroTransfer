cmake_minimum_required(VERSION 3.10)

project(RetroTransfer VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED True)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

if(MINGW)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
endif()

if(WIN32)
    set(NETWORK_LIBS ws2_32)
else()
    set(NETWORK_LIBS "")
endif()

include_directories(include)

set(RETRO_STORAGE_SOURCES
    src/storage/json_helper.cpp
    src/storage/chunk_record.cpp
    src/storage/chunk_verifier.cpp
    src/storage/chunk_storage.cpp
    src/storage/chunk_index.cpp
    src/storage/storage_manager.cpp
)

set(RETRO_CORE_SOURCES
    src/scanner.cpp
    src/benchmark/benchmark_reporter.cpp
    src/fingerprint.c
    src/huffman_codec.c
    src/compression_engine.c
    src/compression_engine.cpp
    src/reconstruction_engine.cpp
    src/fixed_chunker.cpp
    src/cdc_chunker.cpp
    src/adaptive_policy.cpp
    src/chunk_engine.cpp
    src/manifest_writer.cpp
    ${RETRO_STORAGE_SOURCES}
)

set(RETRO_NETWORK_SOURCES
    src/network/transfer_queue.c
    src/network/socket_common.cpp
    src/network/protocol.cpp
    src/network/tcp_client.cpp
    src/network/tcp_server.cpp
    src/network/transfer_sender.cpp
    src/network/transfer_receiver.cpp
)

add_executable(RetroTransfer 
    src/main.cpp 
    ${RETRO_CORE_SOURCES}
    ${RETRO_NETWORK_SOURCES}
)
target_link_libraries(RetroTransfer PRIVATE ${NETWORK_LIBS})

add_executable(RetroTransferSetupCLI
    examples/transfer_setup_cli.cpp
)
add_dependencies(RetroTransferSetupCLI RetroTransfer)

enable_testing()

# Existing chunking engine test
add_executable(test_chunking
    tests/test_chunking.cpp
    ${RETRO_CORE_SOURCES}
)
add_test(NAME ChunkingEngineTests COMMAND test_chunking)

# 1. C Transfer Queue Unit Tests
add_executable(test_transfer_queue
    tests/test_transfer_queue.c
    src/network/transfer_queue.c
)
add_test(NAME TransferQueueTests COMMAND test_transfer_queue)

# 2. Protocol Serialization & Validation Unit Tests
add_executable(test_protocol
    tests/test_protocol.cpp
    src/network/protocol.cpp
    src/network/socket_common.cpp
)
target_link_libraries(test_protocol PRIVATE ${NETWORK_LIBS})
add_test(NAME ProtocolTests COMMAND test_protocol)

# 3. Localhost Socket Communication & Handshake Tests
add_executable(test_network_local
    tests/test_network_local.cpp
    ${RETRO_CORE_SOURCES}
    ${RETRO_NETWORK_SOURCES}
)
target_link_libraries(test_network_local PRIVATE ${NETWORK_LIBS})
add_test(NAME NetworkLocalTests COMMAND test_network_local)

# 4. File Streaming and End-to-End Pipeline & Reconstruction Tests
add_executable(test_file_transfer
    tests/test_file_transfer.cpp
    ${RETRO_CORE_SOURCES}
    ${RETRO_NETWORK_SOURCES}
)
target_link_libraries(test_file_transfer PRIVATE ${NETWORK_LIBS})
add_test(NAME FileTransferAndE2ETests COMMAND test_file_transfer)

# 5. Persistent Storage & Indexing Tests
add_executable(test_storage
    tests/test_storage.cpp
    ${RETRO_CORE_SOURCES}
    ${RETRO_NETWORK_SOURCES}
)
target_link_libraries(test_storage PRIVATE ${NETWORK_LIBS})
add_test(NAME StorageTests COMMAND test_storage)

# 6. Remote Deduplication Tests (Section 10 Scenarios 1-7)
add_executable(test_remote_dedup
    tests/test_remote_dedup.cpp
    ${RETRO_CORE_SOURCES}
    ${RETRO_NETWORK_SOURCES}
)
target_link_libraries(test_remote_dedup PRIVATE ${NETWORK_LIBS})
add_test(NAME RemoteDedupTests COMMAND test_remote_dedup)
# 7. Benchmark Reporting Tests
add_executable(test_benchmark
    tests/test_benchmark.cpp
    src/benchmark/benchmark_reporter.cpp
)

add_test(NAME BenchmarkTests COMMAND test_benchmark)