LIBS = $(LIBS) oleaut32.lib ole32.lib

!IFNDEF MY_NO_UNICODE
CFLAGS = $(CFLAGS) -DUNICODE -D_UNICODE
!ENDIF

XPCFLAGS = /D "_USING_V110_SDK71_"
XPLFALGS = /subsystem:console,5.01
!IF "$(WINXP)" == "1"
CFLAGS = $(CFLAGS) $(XPCFLAGS)
!ENDIF

!IFNDEF O
!IFDEF PLATFORM
O=..\..\..\..\..\..\.dep
!ELSE
O=o
!ENDIF
!ENDIF

INCD = ..\..\..\..\..\..\include
BIND = ..\..\..\..\..\..\Release

!IF "$(PLATFORM)" == "x64"
MY_ML = ml64 -Dx64 -WX
!ELSEIF "$(PLATFORM)" == "arm"
MY_ML = armasm -WX
!ELSE
MY_ML = ml -WX
!ENDIF


!IFDEF UNDER_CE
RFLAGS = $(RFLAGS) -dUNDER_CE
!IFDEF MY_CONSOLE
LFLAGS = $(LFLAGS) /ENTRY:mainACRTStartup
!ENDIF
!ELSE
!IFDEF OLD_COMPILER
LFLAGS = $(LFLAGS) -OPT:NOWIN98
!ENDIF
!IF "$(PLATFORM)" != "arm" && "$(PLATFORM)" != "arm64"
CFLAGS = $(CFLAGS) -Gr
!ENDIF
LIBS = $(LIBS) user32.lib advapi32.lib shell32.lib
!ENDIF

!IF "$(PLATFORM)" == "arm"
COMPL_ASM = $(MY_ML) $** $O/$(*B).obj
!ELSE
COMPL_ASM = $(MY_ML) -c -Fo$O/ $**
!ENDIF

CFLAGS = $(CFLAGS) -nologo -c -Zi /Fd$(BIND)\7z.pdb -Fo$O/ -W4 -WX -EHsc -Gy -GR- -GF

!IFDEF MY_DYNAMIC_LINK
CFLAGS = $(CFLAGS) -MD
!ELSE
!IFNDEF MY_SINGLE_THREAD
CFLAGS = $(CFLAGS) -MT
!ENDIF
!ENDIF

!IFNDEF OLD_COMPILER
CFLAGS = $(CFLAGS) -GS- -Zc:forScope -Zc:wchar_t
!IFNDEF UNDER_CE
CFLAGS = $(CFLAGS) -MP2
!IFNDEF PLATFORM
# CFLAGS = $(CFLAGS) -arch:IA32
!ENDIF
!ENDIF
!ELSE
CFLAGS = $(CFLAGS)
!ENDIF

!IFDEF MY_CONSOLE
CFLAGS = $(CFLAGS) -D_CONSOLE
!ENDIF

!IFNDEF UNDER_CE
!IF "$(PLATFORM)" == "arm"
CFLAGS = $(CFLAGS) -D_ARM_WINAPI_PARTITION_DESKTOP_SDK_AVAILABLE
!ENDIF
!ENDIF

!IF "$(PLATFORM)" == "x64"
CFLAGS_O1 = $(CFLAGS) -O1
!ELSE
CFLAGS_O1 = $(CFLAGS) -O1
!ENDIF
CFLAGS_O2 = $(CFLAGS) -O2

LFLAGS = $(LFLAGS) -nologo -OPT:REF -OPT:ICF

!IFNDEF UNDER_CE
LFLAGS = $(LFLAGS) /LARGEADDRESSAWARE
!ENDIF

!IFDEF DEF_FILE
LFLAGS = $(LFLAGS) -DLL -DEF:$(DEF_FILE)
!ELSE
!IF defined(MY_FIXED) && "$(PLATFORM)" != "arm" && "$(PLATFORM)" != "arm64"
LFLAGS = $(LFLAGS) /FIXED
!ELSE
LFLAGS = $(LFLAGS) /FIXED:NO
!ENDIF
# /BASE:0x400000
!ENDIF


# !IF "$(PLATFORM)" == "x64"

!IFDEF SUB_SYS_VER

MY_SUB_SYS_VER=5.02

!IFDEF MY_CONSOLE
LFLAGS = $(LFLAGS) /SUBSYSTEM:console,$(MY_SUB_SYS_VER)
!ELSE
LFLAGS = $(LFLAGS) /SUBSYSTEM:windows,$(MY_SUB_SYS_VER)
!ENDIF

!ENDIF


PROGPATH = $(BIND)\$(PROG)

COMPL_O1   = $(CC) $(CFLAGS_O1) $**
COMPL_O2   = $(CC) $(CFLAGS_O2) $**
COMPL_PCH  = $(CC) $(CFLAGS_O1) -Yc"StdAfx.h" -Fp$O/a.pch $**
COMPL      = $(CC) $(CFLAGS_O1) -Yu"StdAfx.h" -Fp$O/a.pch $**

COMPLB    = $(CC) $(CFLAGS_O1) -Yu"StdAfx.h" -Fp$O/a.pch $<
# COMPLB_O2 = $(CC) $(CFLAGS_O2) -Yu"StdAfx.h" -Fp$O/a.pch $<
COMPLB_O2 = $(CC) $(CFLAGS_O2) $<

CFLAGS_C_ALL = $(CFLAGS_O2) $(CFLAGS_C_SPEC)
CCOMPL_PCH  = $(CC) $(CFLAGS_C_ALL) -Yc"Precomp.h" -Fp$O/a.pch $**
CCOMPL_USE  = $(CC) $(CFLAGS_C_ALL) -Yu"Precomp.h" -Fp$O/a.pch $**
CCOMPL      = $(CC) $(CFLAGS_C_ALL) $**
CCOMPLB     = $(CC) $(CFLAGS_C_ALL) $<


all: $(INCD)\7zc.h \
     $(PROGPATH)   \

clean:
    if exist $O del /q  $O\*.obj $O\*.lib $O\*.exp $O\*.res $O\*.pch 2>nul
    if exist $(PROGPATH) del /q  $(PROGPATH) 2>nul
    if exist $(INCD)\7zc.h del /q  $(INCD)\7zc.h 2>nul

$O:
	if not exist "$O" mkdir "$O" 2>nul
	if not exist $(BIND) mkdir $(BIND)
	
$O\asm:
	if not exist "$O\asm" mkdir "$O\asm"
	
$(INCD)\7zc.h:
	if not exist "$(INCD)" mkdir "$(INCD)"
	@copy ..\..\..\..\C\7zc.h $@ /y >nul

$(PROGPATH): $O $O/asm $(OBJS) $(DEF_FILE)
	lib /nologo -out:$(PROGPATH) $(OBJS)

$O\StdAfx.obj: $(*B).cpp
	$(COMPL_PCH)
