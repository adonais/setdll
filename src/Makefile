##############################################################################
##

ROOT=..
!include "$(ROOT)\system.mak"
##############################################################################

AFLAGS=/nologo /Zi /c /Fl
CFLAGS=/nologo /Zi $(CLIB) /W4 /WX /O1 /utf-8 $(CFLAGS)
LDFLAGS = /nologo /incremental:no /profile /nodefaultlib:oldnames.lib $(LDFLAGS)
LIBS=$(LIBS) kernel32.lib oleaut32.lib user32.lib advapi32.lib oleaut32.lib ole32.lib

DEPS = $(BIND)\detours.lib $(BIND)\7z.lib
LIBS = $(BIND)\detours.lib /WHOLEARCHIVE:$(BIND)\7z.lib $(LIBS)

.cpp{$(OBJD)}.obj:
    $(CC) $(CFLAGS) /Fd$(OBJD)\setdll$(BITS).pdb /Fo$(OBJD)\ /c $<

all: dirs \
    $(BIND)\setdll$(BITS).exe

##############################################################################

clean:
    -rmdir /q /s $(OBJDS) 2>nul

##############################################################################

dirs:
    @if not exist $(BIND) mkdir $(BIND) && echo.   Created $(BIND)
    @if not exist $(OBJD) mkdir $(OBJD) && echo.   Created $(OBJD)

$(OBJD)\setdll.obj : setdll.cpp

$(OBJD)\resource.res : resource.rc
	$(RC) /nologo /D "_UNICODE" /D "UNICODE" /fo$@ %s
	
$(BIND)\setdll$(BITS).exe : $(OBJD)\setdll.obj $(OBJD)\resource.res $(DEPS)
    $(LD) /OUT:$@ $(OBJD)\setdll.obj $(OBJD)\resource.res $(LDFLAGS) $(LIBS)

################################################################# End of File.
