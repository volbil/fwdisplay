ARCHS = armv7
TARGET = iphone:clang:latest:9.0

THEOS ?= $(HOME)/theos

include $(THEOS)/makefiles/common.mk

APPLICATION_NAME = FWDisplay

FWDisplay_FILES = main.m FWDAppDelegate.m FWDViewController.m FWDTCPServer.m
FWDisplay_FRAMEWORKS = UIKit CoreGraphics Foundation
FWDisplay_CFLAGS = -fobjc-arc

include $(THEOS_MAKE_PATH)/application.mk
