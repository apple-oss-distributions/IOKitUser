#
# Generated by the Apple Project Builder.
#
# NOTE: Do NOT change this file -- Project Builder maintains it.
#
# Put all of your customizations in files called Makefile.preamble
# and Makefile.postamble (both optional), and Makefile will include them.
#

NAME = IOKit

PROJECTVERSION = 2.8
PROJECT_TYPE = Framework
English_RESOURCES = Localizable.strings

LANGUAGES = English

HFILES = IOKitLib.h IOCFSerialize.h IOCFUnserialize.h IOCFPlugIn.h\
         IOKitInternal.h IOCFURLAccess.h IOCFBundle.h IOKitLibPrivate.h\
         IODataQueueClient.h iokitmig.h

OTHERLINKED = IOSharedLock.s IOTrap.s

CFILES = IOCFSerialize.c IOCFUnserialize.tab.c IOKitLib.c\
         IOCFURLAccess.c IOCFBundle.c IODataQueueClient.c IOCFPlugIn.c

SUBPROJECTS = adb.subproj audio.subproj pwr_mgt.subproj usb.subproj\
              network.subproj graphics.subproj hidsystem.subproj\
              kext.subproj hid.subproj ps.subproj

OTHERSRCS = Makefile.preamble Makefile Makefile.postamble iokitmig.defs\
            CustomInfo.plist

OTHERLINKEDOFILES = IOSharedLock.o IOTrap.o

MAKEFILEDIR = $(MAKEFILEPATH)/pb_makefiles
CURRENTLY_ACTIVE_VERSION = YES
DEPLOY_WITH_VERSION_NAME = A
CODE_GEN_STYLE = DYNAMIC
MAKEFILE = framework.make
NEXTSTEP_INSTALLDIR = $(SYSTEM_LIBRARY_DIR)/Frameworks
WINDOWS_INSTALLDIR = /Library/Frameworks
PDO_UNIX_INSTALLDIR = /Library/Frameworks
LIBS = -lkld -lz
DEBUG_LIBS = $(LIBS)
PROF_LIBS = $(LIBS)


HEADER_PATHS = -I/System/Library/Frameworks/Kernel.framework/Headers
NEXTSTEP_PB_CFLAGS = -Wall -Wno-four-char-constants -DAPPLE -DIOKIT -D_ANSI_C_SOURCE -Dvolatile=__volatile
NEXTSTEP_PB_LDFLAGS = -prebind  -seg_addr_table $(NEXT_ROOT)$(APPLE_INTERNAL_DEVELOPER_DIR)/seg_addr_table
FRAMEWORKS = -framework CoreFoundation \
             -framework SystemConfiguration
PUBLIC_HEADERS = IOKitLib.h IOCFUnserialize.h IOCFSerialize.h\
                 IOCFPlugIn.h IOCFURLAccess.h IOCFBundle.h\
                 IODataQueueClient.h iokitmig.h

PROJECT_HEADERS = IOKitLib.h IOCFSerialize.h IOCFUnserialize.h\
                  IOKitLibPrivate.h IOCFURLAccess.h IOCFBundle.h\
                  IODataQueueClient.h iokitmig.h



NEXTSTEP_BUILD_OUTPUT_DIR = /$(USER)/build/$(NAME)

NEXTSTEP_OBJCPLUS_COMPILER = /usr/bin/cc
WINDOWS_OBJCPLUS_COMPILER = $(DEVDIR)/gcc
PDO_UNIX_OBJCPLUS_COMPILER = $(NEXTDEV_BIN)/gcc
NEXTSTEP_JAVA_COMPILER = /usr/bin/javac
WINDOWS_JAVA_COMPILER = $(JDKBINDIR)/javac.exe
PDO_UNIX_JAVA_COMPILER = $(JDKBINDIR)/javac

include $(MAKEFILEDIR)/platform.make

-include Makefile.preamble

include $(MAKEFILEDIR)/$(MAKEFILE)

-include Makefile.postamble

-include Makefile.dependencies