@echo off
REM ============================================================
REM  PXView clang-tidy 一键检查脚本
REM  用法: 双击运行 或 在终端执行 clang-tidy-check.bat
REM  结果输出到: clang-tidy-full-report.txt
REM ============================================================

cd /d "%~dp0"

echo ============================================
echo  PXView clang-tidy 全项目并行检查
echo ============================================
echo.

REM 检查 compile_commands.json 是否存在
if not exist "build\compile_commands.json" (
    echo [!] compile_commands.json 不存在，正在生成...
    pushd build
    cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON .. 2>nul
    popd
    echo [OK] compile_commands.json 已生成
    echo.
)

REM 使用 run-clang-tidy 并行检查所有 PXView 源文件
REM -j 4: 4个并行进程
REM -extra-arg=...: 传递 MinGW 兼容参数给 clang-tidy
echo 开始并行检查 (4 线程)...
echo 结果将保存到: clang-tidy-full-report.txt
echo.

python "C:\Users\admin\scoop\apps\llvm\current\bin\run-clang-tidy" ^
    -p build ^
    -j 4 ^
    "-extra-arg=-target" ^
    "-extra-arg=x86_64-w64-mingw32" ^
    "-extra-arg=--gcc-toolchain=C:/msys64/mingw64" ^
    "-extra-arg=-Wno-ignored-gch" ^
    "-extra-arg=-Wno-unused-command-line-argument" ^
    "PXView" ^
    > clang-tidy-full-report.txt 2>&1

echo.
echo ============================================
echo  检查完成！
echo  报告文件: clang-tidy-full-report.txt
echo ============================================

REM 统计结果
python -c "import re; from collections import Counter; f=open('clang-tidy-full-report.txt','r',encoding='utf-8',errors='replace'); c=f.read(); f.close(); w=re.findall(r'\[(.+?)\]',c); cc=Counter(w); print(f'\nTotal warnings: {len(w)}'); print('\nTop 20:'); [print(f'  {n:5d}  {name}') for name,n in cc.most_common(20)]"

pause
