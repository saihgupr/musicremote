THEOS ?= /opt/theos
export THEOS

# Rootless (Dopamine) settings must be exported before common.mk, otherwise
# Theos stages rootful paths and the package installs to the wrong place.
export THEOS_PACKAGE_SCHEME = rootless
export ARCHS = arm64 arm64e
export TARGET = iphone:clang:latest:15.0
export THEOS_PACKAGE_COMPRESSION = gzip

FINALPACKAGE = 1
INSTALL_TARGET_USER = mobile
THEOS_DEVICE_IP = iphone.local

include $(THEOS)/makefiles/common.mk

TWEAK_NAME = MusicRemote
MusicRemote_FILES = Tweak.xm
MusicRemote_FRAMEWORKS = Foundation UIKit MediaPlayer
MusicRemote_CFLAGS = -fobjc-arc -Wall -Wextra

TOOL_NAME = musicremote
musicremote_FILES = cli/main.c
musicremote_INSTALL_PATH = /usr/bin
musicremote_FRAMEWORKS = CoreFoundation
musicremote_LIBRARIES = sqlite3

include $(THEOS_MAKE_PATH)/tweak.mk
include $(THEOS_MAKE_PATH)/tool.mk

after-install::
	install.exec "killall -9 Music 2>/dev/null || true"
