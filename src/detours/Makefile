##############################################################################
##
##  Makefile for Detours.
##
##  Microsoft Research Detours Package, Version 4.0.1
##
##  Copyright (c) Microsoft Corporation.  All rights reserved.
##

ROOT = ..\..
!include "$(ROOT)\system.mak"

!IF "$(DETOURS_SOURCE_BROWSING)" == ""
DETOURS_SOURCE_BROWSING = 0
!ENDIF

#######################/#######################################################
##
CFLAGS=/W4 /WX /Zi /MT /Gy /Gm- /Zl /Od /utf-8

!IF $(DETOURS_SOURCE_BROWSING)==1
CFLAGS=$(CFLAGS) /FR
!ELSE
CFLAGS=$(CFLAGS) /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x501
!ENDIF

!IF "$(DETOURS_TARGET_PROCESSOR)" == "IA64"
CFLAGS=$(CFLAGS) /wd4163 # intrinsic rdtebex not available; using newer Windows headers with older compiler
!ENDIF

!if defined(DETOURS_WIN_7) && defined(DETOURS_CL_17_OR_NEWER)
CFLAGS=$(CFLAGS) /D_USING_V110_SDK71_
!elseif defined(DETOURS_ANALYZE)
CFLAGS=$(CFLAGS) /analyze
!endif

OBJS = \
    $(OBJD)\detours.obj     \
    $(OBJD)\modules.obj     \
    $(OBJD)\disasm.obj      \
    $(OBJD)\image.obj       \
    $(OBJD)\creatwth.obj    \
    $(OBJD)\disolx86.obj    \
    $(OBJD)\disolx64.obj    \
    $(OBJD)\disolia64.obj   \
    $(OBJD)\disolarm.obj    \
    $(OBJD)\disolarm64.obj  \

##############################################################################
##
.SUFFIXES: .cpp .h .obj

.cpp{$(OBJD)}.obj:
    $(CC) $(CFLAGS) /Fd$(BIND)\detours.pdb /Fo$(OBJD)\ /c $<

##############################################################################

all: dirs \
    $(BIND)\detours.lib \
    $(INCD)\detours.h   \
    $(INCD)\detver.h    \

##############################################################################

clean:
    -rmdir /q /s $(OBJDS) 2>nul

##############################################################################

dirs:
    @if not exist "$(INCD)" mkdir "$(INCD)" && echo.   Created $(INCD)
    @if not exist "$(BIND)" mkdir "$(BIND)" && echo.   Created $(BIND)
    @if not exist "$(OBJD)" mkdir "$(OBJD)" && echo.   Created $(OBJD)

$(BIND)\detours.lib : $(OBJS)
    link /lib /out:$@ $(OBJS)

$(INCD)\detours.h : detours.h
    @copy detours.h $@ /y

$(INCD)\detver.h : detver.h
    @copy detver.h $@ /y

$(OBJD)\detours.obj : detours.cpp detours.h
$(OBJD)\modules.obj : modules.cpp detours.h
$(OBJD)\disasm.obj : disasm.cpp detours.h
$(OBJD)\image.obj : image.cpp detours.h
$(OBJD)\creatwth.obj : creatwth.cpp uimports.cpp detours.h
$(OBJD)\disolx86.obj: disasm.cpp detours.h
$(OBJD)\disolx64.obj: disasm.cpp detours.h
$(OBJD)\disolia64.obj: disasm.cpp detours.h
$(OBJD)\disolarm.obj: disasm.cpp detours.h
$(OBJD)\disolarm64.obj: disasm.cpp detours.h

################################################################# End of File.
