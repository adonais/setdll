##############################################################################
##

ROOT=..
!include "$(ROOT)\system.mak"
##############################################################################

AFLAGS=-nologo -Zi -c -Fl
CFLAGS=$(CLIB) -W4 -WX $(CFLAGS) -Zi -Fd$(BIND)\setdll$(BITS).pdb -D_CRT_SECURE_NO_WARNINGS -std:c++latest
LDFLAGS = -nologo -incremental:no -nodefaultlib:oldnames.lib $(LDFLAGS)
LIBS=$(LIBS) kernel32.lib oleaut32.lib user32.lib advapi32.lib oleaut32.lib ole32.lib $(BIND)\detours.lib

!IF "$(CC)" == "cl"
CFLAGS = $(CFLAGS) -EHsc
!ENDIF

DEPS = $(BIND)\detours.lib

!IF "$(GZIP)" == "1"
CFLAGS = $(CFLAGS) -D_GZIP
DEPS = $(DEPS) $(BIND)\gzip.lib
LIBS = $(LIBS) $(BIND)\gzip.lib
PAK_OBJ = $(OBJD)\pakfile.obj
PAK_CPP = pakfile.cpp
PAK_H = pakfile.h
!ENDIF

!IF "$(7ZIP)" == "1"
CFLAGS = $(CFLAGS) -D_7ZIP
DEPS = $(DEPS) $(BIND)\7z.lib
LIBS = -WHOLEARCHIVE:$(BIND)\7z.lib $(LIBS)
!ENDIF

.cpp{$(OBJD)}.obj:
    $(CC) $(CFLAGS) -Fo$(OBJD)\ -c $<

all: dirs \
    $(BIND)\setdll$(BITS).exe

##############################################################################

clean:
    -rmdir /q /s $(OBJDS) 2>nul

##############################################################################

dirs:
    @if not exist $(BIND) mkdir $(BIND) && echo.   Created $(BIND)
    @if not exist $(OBJD) mkdir $(OBJD) && echo.   Created $(OBJD)

$(OBJD)\setdll.obj : setdll.cpp $(PAK_H)

!IF "$(GZIP)" == "1"
$(OBJD)\pakfile.obj : $(PAK_CPP) $(PAK_H)
    $(CC) $(CFLAGS) -Fo$(OBJD)\ -c $(PAK_CPP)
!ENDIF

$(OBJD)\resource.res : resource.rc
	$(RC) -nologo -D "_UNICODE" -D "UNICODE" -fo$@ %s

$(BIND)\setdll$(BITS).exe : $(OBJD)\setdll.obj $(PAK_OBJ) $(OBJD)\resource.res $(DEPS)
    $(LD) -OUT:$@ $(OBJD)\setdll.obj $(PAK_OBJ) $(OBJD)\resource.res $(LDFLAGS) $(LIBS)

################################################################# End of File.
