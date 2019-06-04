##############################################################################
##
##  Makefile for Detours Test Programs.
##
##  Microsoft Research Detours Package
##
##  Copyright (c) Microsoft Corporation.  All rights reserved.
##

!include ..\common.mak

LIBS=$(LIBS) kernel32.lib

all: dirs \
    $(BIND)\setdll$(BITS).exe \
!IF $(DETOURS_SOURCE_BROWSING)==1
    $(OBJD)\setdll.bsc \
!ENDIF
	option

##############################################################################

clean:
    -rmdir /q /s $(OBJDS) 2>nul

##############################################################################

dirs:
    @if not exist $(BIND) mkdir $(BIND) && echo.   Created $(BIND)
    @if not exist $(OBJD) mkdir $(OBJD) && echo.   Created $(OBJD)

$(OBJD)\setdll.obj : setdll.cpp

$(OBJD)\resource.res : resource.rc
	rc.exe /nologo /D "_UNICODE" /D "UNICODE" /fo$@ %s
	
$(BIND)\setdll$(BITS).exe : $(OBJD)\setdll.obj $(OBJD)\resource.res $(DEPS)
    cl $(CFLAGS) /Fe$@ /Fd$(@R).pdb $(OBJD)\setdll.obj $(OBJD)\resource.res \
        /link $(LINKFLAGS) $(LIBS) /subsystem:console

$(OBJD)\setdll.bsc : $(OBJD)\setdll.obj
    bscmake /v /n /o $@ $(OBJD)\setdll.sbr

############################################### Install non-bit-size binaries.

option:

##############################################################################

test: all
    @echo -------- Reseting test binaries to initial state. -----------------------

################################################################# End of File.
