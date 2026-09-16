# 测试目标。
#
# GoogleTest 的解析已移到顶层 CMakeLists.txt，因为 src/gateway 需要先知道
# GTest 是否可用才能决定是否加入自己的测试目标。

if(NOT RGBT_TESTS_AVAILABLE)
  return()
endif()

add_subdirectory(unit)
