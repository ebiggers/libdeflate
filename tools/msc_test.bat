call d:\VS2015\vc\vcvarsall
nmake /f Makefile.msc clean
nmake /f Makefile.msc
cl /MD /O2 /Fe:benchmark.exe -I. -Itools -I..\zlib-msc tools\benchmark.c tools\wgetopt.c libdeflatestatic.lib ..\zlib-msc\zlib.lib
cl /MD /O2 /Fe:gzip.exe -I. -Itools tools\gzip.c tools\wgetopt.c libdeflatestatic.lib
del j:\exe\gzip.exe 2> nul
del j:\exe\gunzip.exe 2> nul
copy gzip.exe j:\exe\gzip.exe
copy gzip.exe j:\exe\gunzip.exe
del j:\exe\benchmark.exe 2> nul
copy benchmark.exe j:\exe\benchmark.exe

call d:\VS2015\vc\vcvarsall x86_amd64
nmake /f Makefile.msc clean
nmake /f Makefile.msc
cl /MD /O2 /Fe:benchmark.exe -I. -Itools -I..\zlib-msc64 tools\benchmark.c tools\wgetopt.c libdeflatestatic.lib ..\zlib-msc64\zlib.lib
cl /MD /O2 /Fe:gzip.exe -I. -Itools tools\gzip.c tools\wgetopt.c libdeflatestatic.lib
del j:\exe64\gzip.exe 2> nul
del j:\exe64\gunzip.exe 2> nul
copy gzip.exe j:\exe64\gzip.exe
copy gzip.exe j:\exe64\gunzip.exe
del j:\exe64\benchmark.exe 2> nul
copy benchmark.exe j:\exe64\benchmark.exe
