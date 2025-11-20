LOCAL_PATH := $(call my-dir)


# 主模块
include $(CLEAR_VARS)
LOCAL_MODULE    := Lark

# 编译选项（合并并移除冲突选项）
LOCAL_CFLAGS    := -std=c++17
LOCAL_CPPFLAGS  := -std=c++17
# C++ 标准与编译选项
LOCAL_CPPFLAGS := -std=c++17
LOCAL_CPPFLAGS += -O2
LOCAL_CFLAGS := -O2

# 头文件路径（按模块分层，确保正确性）
LOCAL_C_INCLUDES += \
    $(LOCAL_PATH)/include/ \
     $(LOCAL_PATH)/include/PtraceManager.h \
    $(LOCAL_PATH)/usr/include/  # 系统头文件路径
# 源文件列表（按模块分组，清晰可维护）
LOCAL_SRC_FILES := \
        src/main.cpp \
        include/SyscallMemory.h \
        include/DriverMemory.h \
        include/DriverTest.h \
# 链接库
LOCAL_LDLIBS := -llog -landroid -lEGL -lGLESv1_CM -lGLESv2 -lGLESv3 -lc
# 构建可执行文件
include $(BUILD_EXECUTABLE)
