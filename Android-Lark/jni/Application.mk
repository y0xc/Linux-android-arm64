# 目标 CPU 架构
APP_ABI := arm64-v8a

# 目标 Android API 级别, 21 是支持 process_vm_writev 的最低级别
APP_PLATFORM := android-21

# 使用静态 C++ 标准库
APP_STL := c++_static

# 优化级别 (可选, 'release' 或 'debug')
APP_OPTIM := release