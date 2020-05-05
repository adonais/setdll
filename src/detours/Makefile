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
#######################/#######################################################
##
CFLAGS=/nologo /W4 /WX /Zi /MT /Gy /Zl /Od /utf-8 $(CFLAGS) 

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
    -del /q /s $(OBJS) $(BIND)\detours.lib $(BIND)\detours.pdb 2>nul

##############################################################################

dirs:
    @if not exist "$(INCD)" mkdir "$(INCD)" && echo.   Created $(INCD)
    @if not exist "$(BIND)" mkdir "$(BIND)" && echo.   Created $(BIND)
    @if not exist "$(OBJD)" mkdir "$(OBJD)" && echo.   Created $(OBJD)

$(BIND)\detours.lib : $(OBJS)
    $(AR) /out:$@ $(OBJS)

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
