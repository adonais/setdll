## System requirements

C compiler 

Microsoft Visual Studio 2013 or above .

## build

nmake clean 
nmake 

or

nmake CC=clang-cl clean 
nmake CC=clang-cl 

## About setdll

Usage:
    setdll [options] binary_files
    
Options:

    /d:file.dll         :        Add file.dll binary files
    
    /r                  :        Remove extra DLLs from binary files
    
    /p:browser\omni.ja  :        Repair omni.ja to support Upcheck.exe

    /t:file.exe         :        Test PE file bits
        
    /?                  :        This help screen
    
    -7 --help           :        7z command help screen
    
