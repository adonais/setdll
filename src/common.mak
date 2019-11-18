##############################################################################

!IF "$(ROOT)" == ""
ROOT = ..\..
!ENDIF
!include "$(ROOT)\system.mak"

!IF "$(DETOURS_SOURCE_BROWSING)" == ""
DETOURS_SOURCE_BROWSING=0
!ENDIF

##############################################################################

!IFNDEF CLIB
CLIB=/MT
!ENDIF

AFLAGS=/nologo /Zi /c /Fl
CFLAGS=/nologo /Zi $(CLIB) /Gm- /W4 /WX /O1 /utf-8 $(CFLAGS)

!IF $(DETOURS_SOURCE_BROWSING)==1
CFLAGS=$(CFLAGS) /FR
!ELSE
CFLAGS=$(CFLAGS) /I$(INCD)
!ENDIF

LIBFLAGS=/nologo
!IF "$(WINXP)" == "1"
LINKFLAGS=/nologo /release /incremental:no /profile /nodefaultlib:oldnames.lib $(LDFLAGS)
!ELSE
LINKFLAGS=/nologo /release /incremental:no /profile /nodefaultlib:oldnames.lib /subsystem:console
!ENDIF

!if defined(DETOURS_WIN_7) && defined(DETOURS_CL_17_OR_NEWER)
CFLAGS=$(CFLAGS) /D_USING_V110_SDK71_
!endif

!IF "$(DETOURS_TARGET_PROCESSOR)" == "X86"
BITS=32
ASM=ml

!ELSEIF "$(DETOURS_TARGET_PROCESSOR)" == "X64"
BITS=64
ASM=ml64

!ELSEIF "$(DETOURS_TARGET_PROCESSOR)" == "IA64"

ASM=ias
AFLAGS=-F COFF32_PLUS
CFLAGS=$(CFLAGS) /wd4163 # intrinsic rdtebex not available; using newer Windows headers with older compiler
#CFLAGS=$(CFLAGS) /wd4996 /wd4068

!ELSEIF "$(DETOURS_TARGET_PROCESSOR)" == "ARM"

ASM=armasm
AFLAGS=-coff_thumb2_only
CFLAGS=$(CFLAGS) /D_ARM_WINAPI_PARTITION_DESKTOP_SDK_AVAILABLE

CFLAGS=$(CFLAGS) /D_$(DETOURS_TARGET_PROCESSOR:X64=AMD64)_ # redundant with windows.h except for midl proxies

!ENDIF

DEPS = $(BIND)\detours.lib $(BIND)\7z.lib
LIBS = $(BIND)\detours.lib /WHOLEARCHIVE:$(BIND)\7z.lib

################################################################# End of File.
