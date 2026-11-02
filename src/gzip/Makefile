ROOT = ..\..
!include "$(ROOT)\system.mak"

#######################/#######################################################
##
CFLAGS=-W4 -WX -Gy $(CFLAGS) -Zi -Fd$(BIND)\gzip.pdb

OBJS = \
    $(OBJD)\miniz.obj         \
    $(OBJD)\mini_gzip.obj     \

##############################################################################
##
.SUFFIXES: .c .h .obj

.c{$(OBJD)}.obj:
    $(CC) $(CFLAGS) -Fo$(OBJD)\ -c $<

##############################################################################

all: dirs \
    $(BIND)\gzip.lib      \
    $(INCD)\mini_gzip.h   \

##############################################################################

clean:
    -del /q /s $(OBJS) $(BIND)\gzip.lib $(BIND)\gzip.pdb 2>nul

##############################################################################

dirs:
    @if not exist "$(INCD)" mkdir "$(INCD)" && echo.   Created $(INCD)
    @if not exist "$(BIND)" mkdir "$(BIND)" && echo.   Created $(BIND)
    @if not exist "$(OBJD)" mkdir "$(OBJD)" && echo.   Created $(OBJD)

$(BIND)\gzip.lib : $(OBJS)
    $(AR) /out:$@ $(OBJS)

$(INCD)\mini_gzip.h : mini_gzip.h
    @copy mini_gzip.h $@ /y

$(OBJD)\miniz.obj : miniz.c miniz.h
$(OBJD)\mini_gzip.obj : mini_gzip.c mini_gzip.h

################################################################# End of File.
