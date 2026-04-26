cmake_minimum_required(VERSION 3.15)
project(Dupic)

# Use C++20 for Designated Initializers support
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Dependencies via vcpkg ---
find_package(SDL3 REQUIRED CONFIG)
find_package(SDL3_ttf REQUIRED CONFIG)
find_package(SDL3_image REQUIRED CONFIG)

# --- External Path for libvips ---
set(VIPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vips")

# --- Define Executable ---
add_executable(${PROJECT_NAME} 
    main.cpp
    layout.cpp
    scan.cpp
    app.rc
)

if(MSVC)
    # /arch:AVX2 enables the AVX2 instruction set
    # /Oi generates intrinsic functions
    target_compile_options(${PROJECT_NAME} PRIVATE /arch:AVX2 /O2 /Oi /Ot /Oy)
else()
    # -mavx2 enables the AVX2 instruction set
    target_compile_options(${PROJECT_NAME} PRIVATE -O3 -mavx2)
endif()

# No console window on Windows
set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)

# --- Include Directories ---
target_include_directories(${PROJECT_NAME} PRIVATE 
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/clay"
    "${VIPS_DIR}/include"
    "${VIPS_DIR}/include/glib-2.0"
    "${VIPS_DIR}/lib/glib-2.0/include"
)

# --- Linking ---
target_link_directories(${PROJECT_NAME} PRIVATE "${VIPS_DIR}/lib")

# Link everything
target_link_libraries(${PROJECT_NAME} PRIVATE 
    SDL3::SDL3
    SDL3_ttf::SDL3_ttf
    SDL3_image::SDL3_image
    libvips 
    libvips-cpp
    libglib-2.0
    libgobject-2.0
)

# --- Post-Build: Copy libvips DLLs ---
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${VIPS_DIR}/bin"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMENT "Copying libvips binaries to output directory..."

# --- Post-Build: Copy SDL3 DLLs ---
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:SDL3::SDL3>"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMENT "Copying SDL3 DLL...")
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:SDL3_ttf::SDL3_ttf>"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMENT "Copying SDL3_ttf DLL...")
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:SDL3_image::SDL3_image>"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMENT "Copying SDL3_image DLL...")

# --- Post-Build: Copy assets ---
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets"
    COMMENT "Copying assets..."))