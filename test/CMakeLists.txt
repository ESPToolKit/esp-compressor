add_library(esp_compressor_core STATIC
    ${PROJECT_SOURCE_DIR}/src/esp_compressor/async/compressor.cpp
    ${PROJECT_SOURCE_DIR}/src/esp_compressor/codec/codec.cpp
    ${PROJECT_SOURCE_DIR}/src/esp_compressor/format/esc_format.cpp
    ${PROJECT_SOURCE_DIR}/src/esp_compressor/io/compression_io.cpp
    ${PROJECT_SOURCE_DIR}/src/esp_compressor/util/fs_utils.cpp
    ${PROJECT_SOURCE_DIR}/src/esp_compressor/util/memory_buffer.cpp
)

target_include_directories(esp_compressor_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/stubs
)

target_compile_features(esp_compressor_core PUBLIC cxx_std_17)
target_compile_definitions(esp_compressor_core PUBLIC ESPCOMPRESSOR_TESTING)

add_executable(esp_compressor_tests
    esp_compressor_tests.cpp
)

target_include_directories(esp_compressor_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/stubs
)

target_link_libraries(esp_compressor_tests
    PRIVATE
        esp_compressor_core
)

target_compile_features(esp_compressor_tests PRIVATE cxx_std_17)

add_test(NAME esp_compressor_tests COMMAND esp_compressor_tests)
