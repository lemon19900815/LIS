:: cd src
@echo off
REM astyle --style=clang --suffix=none include/*.hpp
clang-format -i --verbose include/lis/*.h src/*.cpp tests/*.cpp
:: cd ../ui

:: astyle --style=ansi -s4 --unpad-paren --pad-header --max-instatement-indent=79 --convert-tabs -Y -m0 -p -w -j -k3 -R -Q -W3 --suffix=none *.cpp *.h
:: astyle --style=google --unpad-paren --pad-header --max-instatement-indent=79 --convert-tabs -Y -m0 -p -w -j -k3 -R -Q -W3 --suffix=none *.cpp *.hpp
pause