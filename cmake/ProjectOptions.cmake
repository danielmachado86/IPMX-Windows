include_guard(GLOBAL)

function(ipmx_configure_project_options)
  add_library(ipmx_project_options INTERFACE)
  add_library(ipmx::project_options ALIAS ipmx_project_options)
  target_compile_features(ipmx_project_options INTERFACE cxx_std_20)
  target_compile_definitions(ipmx_project_options INTERFACE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
  )

  add_library(ipmx_project_warnings INTERFACE)
  add_library(ipmx::project_warnings ALIAS ipmx_project_warnings)
  if(MSVC)
    target_compile_options(ipmx_project_warnings INTERFACE
      /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus /sdl
      $<$<BOOL:${IPMX_WARNINGS_AS_ERRORS}>:/WX>
    )
    target_link_options(ipmx_project_options INTERFACE /DYNAMICBASE /NXCOMPAT)
  endif()

  if(IPMX_ENABLE_CLANG_TIDY)
    find_program(IPMX_CLANG_TIDY_EXECUTABLE NAMES clang-tidy REQUIRED)
    set(CMAKE_CXX_CLANG_TIDY "${IPMX_CLANG_TIDY_EXECUTABLE}" CACHE STRING "" FORCE)
  endif()
endfunction()
