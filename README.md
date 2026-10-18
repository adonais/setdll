## System requirements

C compiler 

Microsoft Visual Studio 2013 or above .

## build

nmake clean    
nmake 

or

nmake CC=clang-cl clean  
nmake CC=clang-cl 7ZIP=1 MD=1  

## About setdll

Usage:
    setdll [options] binary_files
    
Options:

    /d:file.dll         :        Add file.dll binary files, successfully returned 0
    
    /r                  :        Remove extra DLLs from binary files, successfully returned 0
    
    /t:file.exe         :        Test PE file bits. return code, 64 or 32
        
    /?                  :        This help screen
    
    -7 --help           :        7z command help screen
    
    Example, extract/compress files :
    
    setdll -7 x d:/msedge++.rar -aoa -o.
    setdll -7 a msedge++.7z -mx9 msedge++.dll msedge++(example).ini
