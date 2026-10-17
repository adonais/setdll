##############################################################################
##

ROOT=..
!include "$(ROOT)\system.mak"
##############################################################################

AFLAGS=-nologo -Zi -c -Fl
CFLAGS=$(CLIB) -W4 -WX $(CFLAGS) -Zi -Fd$(BIND)\setdll$(BITS).pdb
LDFLAGS = -nologo -incremental:no -nodefaultlib:oldnames.lib $(LDFLAGS)
LIBS=$(LIBS) kernel32.lib oleaut32.lib user32.lib advapi32.lib oleaut32.lib ole32.lib

DEPS = $(BIND)\detours.lib $(BIND)\7z.lib
LIBS = $(BIND)\detours.lib -WHOLEARCHIVE:$(BIND)\7z.lib $(LIBS)

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

$(OBJD)\setdll.obj : setdll.cpp

$(OBJD)\resource.res : resource.rc
	$(RC) -nologo -D "_UNICODE" -D "UNICODE" -fo$@ %s

$(BIND)\setdll$(BITS).exe : $(OBJD)\setdll.obj $(OBJD)\resource.res $(DEPS)
    $(LD) -OUT:$@ $(OBJD)\setdll.obj $(OBJD)\resource.res $(LDFLAGS) $(LIBS)

################################################################# End of File.
