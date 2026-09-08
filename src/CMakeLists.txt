find_package(SDL3 3.2.6 REQUIRED)   # the same package the emulator uses
find_package(PNG REQUIRED)          # texture dumps

# The lighting is one text compiled twice: as GLSL here and as C++ by shading_cpu.h, so a CPU check
# cannot drift from the shader it checks.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/render/shading.inc" SDF_SHADING_SOURCE)
configure_file(render/shading_glsl.h.in "${CMAKE_CURRENT_BINARY_DIR}/shading_glsl.h" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS render/shading.inc)

add_executable(sdfhook
	main.cpp
	render/frame.cpp
	render/render_texture.cpp
	render/renderer.cpp
	render/sprite.cpp
	render/render_thread.cpp
	${CMAKE_SOURCE_DIR}/thirdparty/glad/gl.c
	config.cpp
	guest_ptr.cpp
	log.cpp
	gif.cpp
	model.cpp
	image.cpp
	texture.cpp
	unsupported.cpp
	hooks.cpp
	trace.cpp)

target_include_directories(sdfhook PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}"
	"${CMAKE_CURRENT_BINARY_DIR}"
	"${PCSX2HOOK_TREE}/pcsx2-hook/include"
	"${CMAKE_SOURCE_DIR}/thirdparty/glad/include")

set_target_properties(sdfhook PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(sdfhook PRIVATE -Wall -Wextra)

target_link_libraries(sdfhook PRIVATE pcsx2hook SDL3::SDL3 PNG::PNG)

# The emulator looks for resources/ beside the running binary.
add_custom_command(TARGET sdfhook POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E create_symlink
		"${PCSX2HOOK_TREE}/bin/resources" "$<TARGET_FILE_DIR:sdfhook>/resources")
