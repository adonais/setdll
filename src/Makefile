##############################################################################
##

ROOT=..
!include .\common.mak

LIBS=$(LIBS) kernel32.lib oleaut32.lib user32.lib advapi32.lib

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
	rc.exe /nologo /D "_UNICODE" /D "UNICODE" /fo$@ %s
	
$(BIND)\setdll$(BITS).exe : $(OBJD)\setdll.obj $(OBJD)\resource.res $(DEPS)
    link /OUT:$@ $(OBJD)\setdll.obj $(OBJD)\resource.res $(LINKFLAGS) $(LIBS) /subsystem:console

################################################################# End of File.
