cmake_minimum_required (VERSION 3.8.2)
project (ValiTools)
enable_language (C)

set (CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set (CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set (CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Build the diskutility, since this is a c# project it is a bit tricky
add_subdirectory (diskutility)

# target_compile_options(example PUBLIC -std=c++1y -Wall -Wfloat-conversion)
if (MSVC)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
endif ()

# Build the revision file generator utility
add_executable (revision revision/main.c)
install(TARGETS revision EXPORT tools_revision DESTINATION bin)
install(EXPORT tools_revision NAMESPACE rev_ DESTINATION lib/tools_revision)

# Build the ramdisk utility
add_executable (rd rd/main.c)
install(TARGETS rd EXPORT tools_rd DESTINATION bin)
install(EXPORT tools_rd NAMESPACE rd_ DESTINATION lib/tools_rd)

# Build the image compressor utility
add_executable (lzss lzss/main.c)
if (MSVC)
    target_link_libraries (lzss PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/lzss/aplib64.lib)
else ()
    target_link_libraries (lzss PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/lzss/aplib64.a)
endif ()
install(TARGETS lzss EXPORT tools_lz DESTINATION bin)
install(EXPORT tools_lz NAMESPACE lz_ DESTINATION lib/tools_lz)

# Build the file to C-hex array utility
add_executable (file2c file2c/main.c)
install(TARGETS file2c EXPORT tools_f2c DESTINATION bin)
install(EXPORT tools_f2c NAMESPACE f2c_ DESTINATION lib/tools_f2c)
