##############################################################################

ROOT = .
!include "$(ROOT)\system.mak"

all:
    cd "$(MAKEDIR)"
	@if exist "$(MAKEDIR)\src\7z\CPP\7zip\Bundles\Alone" cd "$(MAKEDIR)\src\7z\CPP\7zip\Bundles\Alone" && $(MAKE) /NOLOGO /$(MAKEFLAGS)
	@if exist "$(MAKEDIR)\src\detours\makefile" cd "$(MAKEDIR)\src\detours" && $(MAKE) /NOLOGO /$(MAKEFLAGS)
    cd "$(MAKEDIR)\src"
    @$(MAKE) /NOLOGO /$(MAKEFLAGS)
    cd "$(MAKEDIR)"

clean:
    cd "$(MAKEDIR)"
	@if exist "$(MAKEDIR)\src\7z\CPP\7zip\Bundles\Alone" cd "$(MAKEDIR)\src\7z\CPP\7zip\Bundles\Alone" && $(MAKE) /NOLOGO /$(MAKEFLAGS) clean
	@if exist "$(MAKEDIR)\src\detours\makefile" cd "$(MAKEDIR)\src\detours" && $(MAKE) /NOLOGO /$(MAKEFLAGS) clean
    cd "$(MAKEDIR)\src"
    @$(MAKE) /NOLOGO /$(MAKEFLAGS) clean
    cd "$(MAKEDIR)"
    -rmdir /q /s $(INCD) 2> nul
    -rmdir /q /s $(LIBD) 2> nul
    -rmdir /q /s $(OBJD) 2> nul
    -rmdir /q /s $(BIND) 2> nul
    -rmdir /q /s dist 2> nul
    -del docsrc\detours.chm 2> nul
    -del /q *.msi 2>nul
    -del /q /f /s *~ 2>nul

################################################################# End of File.
