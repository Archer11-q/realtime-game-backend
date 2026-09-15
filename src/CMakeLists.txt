add_subdirectory(common)

# gateway 依赖 vcpkg 提供的 brpc。默认构建（debug / release / asan）不包含
# vcpkg，因此这里只在启用 vcpkg 工具链时加入，保证既有预设与 CI 不受影响。
if(RGBT_USE_VCPKG_TOOLCHAIN)
  add_subdirectory(gateway)
endif()
