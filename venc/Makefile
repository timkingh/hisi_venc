# Hisilicon Hi35xx sample Makefile

include ../Makefile.param

SMP_SRCS := $(wildcard *.cpp)
TARGET := $(SMP_SRCS:%.cpp=%)

TARGET_PATH := $(PWD)

# compile linux or HuaweiLite
include $(PWD)/../$(ARM_ARCH)_$(OSTYPE).mak
