# Native Win32 project: no browser engine or background service.
cmake_minimum_required(VERSION 3.22)
project(NodeSight VERSION 1.0.0 LANGUAGES CXX RC)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT WIN32)
  message(FATAL_ERROR "Node-Sight is a native Windows desktop widget.")
endif()
# GUI subsystem; resources embed the manifest and Linux collector.
add_executable(NodeSight WIN32 src/main.cpp src/metrics.cpp src/remote.cpp src/config.cpp src/app.rc)
target_compile_definitions(NodeSight PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(NodeSight PRIVATE user32 gdi32 shell32 advapi32 iphlpapi psapi comctl32 comdlg32 dwmapi)
if(MSVC)
  # app.rc supplies the manifest; avoid a duplicate linker resource.
  target_link_options(NodeSight PRIVATE /MANIFEST:NO)
endif()
# Small helper supplies saved secrets only through OpenSSH askpass.
add_executable(node-sight-askpass WIN32 src/askpass.cpp)
target_compile_definitions(node-sight-askpass PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(node-sight-askpass PRIVATE advapi32)
# Regression tests do not need a real remote SSH server.
add_executable(metrics-tests tests/metrics_test.cpp src/metrics.cpp)
target_compile_definitions(metrics-tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(metrics-tests PRIVATE src)
target_link_libraries(metrics-tests PRIVATE iphlpapi)
foreach(target NodeSight node-sight-askpass metrics-tests)
  if(MSVC)
    # Static CRT removes the separate VC redistributable installation requirement.
    set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(${target} PRIVATE /W4 /utf-8 /permissive- /EHsc)
    target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:/O1> $<$<CONFIG:Release>:/GL>)
    target_link_options(${target} PRIVATE $<$<CONFIG:Release>:/LTCG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
  endif()
endforeach()
enable_testing()
add_test(NAME metrics COMMAND metrics-tests)
# Stage executables only, never settings, keys or cached diagnostics.
install(TARGETS NodeSight node-sight-askpass RUNTIME DESTINATION .)
