ROOT = ..\..\..\..\..\..
!include "$(ROOT)\system.mak"
#######################/#######################################################

!IF "$(PLATFORM)" == "x64"
MY_ML = ml64 -Dx64 -WX
!ELSEIF "$(PLATFORM)" == "arm"
MY_ML = armasm -WX
!ELSE
MY_ML = ml -WX
!ENDIF


CFLAGS = -nologo -c -O1 -Zi /Fd$(BIND)\7z.pdb -Fo$O\ -EHsc -Gy -GR- -GF $(CFLAGS)
O=..\..\..\..\..\..\.dep
LFLAGS = $(LDFLAGS) -nologo -OPT:REF -OPT:ICF
PROGPATH = $(BIND)\$(PROG)

!IF "$(COMPILER_MSVC)" == "1"
COMPL_PCH  = $(CC) $(CFLAGS) -Yc"StdAfx.h" -Fp$O\a.pch $**
COMPL      = $(CC) $(CFLAGS) -Yu"StdAfx.h" -Fp$O\a.pch $**
COMPLB    = $(CC) $(CFLAGS) -Yu"StdAfx.h" -Fp$O\a.pch $<
CCOMPL      = $(CC) $(CFLAGS) $**
CCOMPLB     = $(CC) $(CFLAGS) $<
!ELSE
COMPL_PCH  = $(CC) $(CFLAGS) -Yc"StdAfx.h" -Fp$O\a.pch $**
COMPL      = $(CC) $(CFLAGS) $**
COMPLB    = $(CC) $(CFLAGS) $<
CCOMPL      = $(CC) $(CFLAGS) $**
CCOMPLB     = $(CC) $(CFLAGS) $<
!ENDIF

!IF "$(PLATFORM)" == "arm"
COMPL_ASM = $(ASM) $(AFLAGS) $** $O/$(*B).obj
!ELSE
COMPL_ASM = $(ASM) $(AFLAGS) -c -Fo$O/ $**
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
	$(AR) -out:$(PROGPATH) $(OBJS)

$O\StdAfx.obj: $(*B).cpp
	$(COMPL_PCH)
