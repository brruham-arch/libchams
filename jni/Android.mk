LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE    := chams
LOCAL_SRC_FILES := ../mod/main.cpp

LOCAL_C_INCLUDES := \
    $(LOCAL_PATH)/../include \
    $(LOCAL_PATH)/../include/mod

LOCAL_CPPFLAGS := \
    -std=c++17 \
    -O2 \
    -fPIC \
    -fvisibility=hidden \
    -ffunction-sections \
    -fdata-sections \
    -mthumb

LOCAL_LDLIBS := \
    -llog \
    -lm \
    -ldl

LOCAL_LDFLAGS := \
    -static-libstdc++ \
    -Wl,--gc-sections

include $(BUILD_SHARED_LIBRARY)
