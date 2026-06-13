@echo off
REM run_tests.cmd - Batch test runner for C decoder tests
REM
REM Usage:
REM   run_tests.cmd [decoder_name] [--tolerance N] [--testdata-dir <path>]
REM
REM Exit code: 0 if all tests pass, 1 if any fail

setlocal enabledelayedexpansion

REM ---- Defaults ----
set TOLERANCE=0
set TESTDATA_DIR=./testdata
set FILTER_DECODER=

REM ---- Parse arguments ----
:parse_args
if "%~1"=="" goto :done_args
if /i "%~1"=="--tolerance" (
    set TOLERANCE=%~2
    shift
    shift
    goto :parse_args
)
if /i "%~1"=="--testdata-dir" (
    set TESTDATA_DIR=%~2
    shift
    shift
    goto :parse_args
)
if "%~1"=="--" (
    shift
    goto :done_args
)
REM Assume it's the decoder name if not starting with -
echo %~1 | findstr /r "^-" >nul
if errorlevel 1 (
    if "!FILTER_DECODER!"=="" (
        set FILTER_DECODER=%~1
        shift
        goto :parse_args
    ) else (
        echo ERROR: Unexpected argument: %~1
        exit /b 2
    )
) else (
    echo ERROR: Unknown option: %~1
    exit /b 2
)
:done_args

REM ---- Find decoder_test executable ----
set DECODER_TEST=

if exist ".\decoder_test.exe" (
    set DECODER_TEST=.\decoder_test.exe
    goto :found_exe
)
if exist ".\decoder_test" (
    set DECODER_TEST=.\decoder_test
    goto :found_exe
)
if exist "..\build.dir\bin\decoder_test.exe" (
    set DECODER_TEST=..\build.dir\bin\decoder_test.exe
    goto :found_exe
)
if exist "..\build.dir\bin\decoder_test" (
    set DECODER_TEST=..\build.dir\bin\decoder_test
    goto :found_exe
)
if exist "..\build.dir\decoder_test.exe" (
    set DECODER_TEST=..\build.dir\decoder_test.exe
    goto :found_exe
)
if exist "..\build.dir\decoder_test" (
    set DECODER_TEST=..\build.dir\decoder_test
    goto :found_exe
)
if exist "..\build\bin\decoder_test.exe" (
    set DECODER_TEST=..\build\bin\decoder_test.exe
    goto :found_exe
)
if exist "..\build\bin\decoder_test" (
    set DECODER_TEST=..\build\bin\decoder_test
    goto :found_exe
)

REM Check PATH
where decoder_test.exe >nul 2>&1
if not errorlevel 1 (
    set DECODER_TEST=decoder_test.exe
    goto :found_exe
)

echo ERROR: decoder_test executable not found.
echo Searched: .\, ..\build.dir\bin\, ..\build.dir\, ..\build\bin\, PATH
exit /b 2

:found_exe
echo Using decoder_test: %DECODER_TEST%
echo Testdata directory: %TESTDATA_DIR%
echo Tolerance: %TOLERANCE%
echo.

REM ---- Counters ----
set PASS_COUNT=0
set FAIL_COUNT=0
set SKIP_COUNT=0
set ERROR_COUNT=0

REM ---- Build tolerance arg ----
set TOLERANCE_ARG=
if not "%TOLERANCE%"=="0" set TOLERANCE_ARG=--tolerance %TOLERANCE%

REM ---- Collect all known C decoder names from source ----
set C_DECODERS_DIR=..\c_decoders
set "ALL_DECODERS="

REM ---- Determine which decoders to test ----
if not "!FILTER_DECODER!"=="" (
    set DECODERS_TO_TEST=!FILTER_DECODER!
) else (
    REM Build list from c_decoders source directory
    set "DECODERS_TO_TEST="
    if exist "!C_DECODERS_DIR!\*_c.c" (
        for %%f in ("!C_DECODERS_DIR!\*_c.c") do (
            set dname=%%~nf
            if "!DECODERS_TO_TEST!"=="" (
                set "DECODERS_TO_TEST=!dname!"
            ) else (
                set "DECODERS_TO_TEST=!DECODERS_TO_TEST! !dname!"
            )
        )
    )
    REM Fall back to testdata subdirs
    if "!DECODERS_TO_TEST!"=="" (
        if exist "!TESTDATA_DIR!" (
            for /d %%d in ("!TESTDATA_DIR!\*") do (
                set dname=%%~nd
                if "!DECODERS_TO_TEST!"=="" (
                    set "DECODERS_TO_TEST=!dname!"
                ) else (
                    set "DECODERS_TO_TEST=!DECODERS_TO_TEST! !dname!"
                )
            )
        )
    )
)

REM ---- Temp files for failed/skipped lists ----
set "FAILED_FILE=%TEMP%\run_tests_failed_%RANDOM%.txt"
set "SKIPPED_FILE=%TEMP%\run_tests_skipped_%RANDOM%.txt"
type nul > "!FAILED_FILE!"
type nul > "!SKIPPED_FILE!"

REM ---- Run tests ----
for %%D in (!DECODERS_TO_TEST!) do (
    set "decoder=%%D"
    set "decoder_dir=!TESTDATA_DIR!\!decoder!"

    REM Check if testdata directory exists
    if not exist "!decoder_dir!\" (
        set /a SKIP_COUNT+=1
        echo !decoder!>> "!SKIPPED_FILE!"
    ) else (
        REM Find test case subdirectories
        set "found_case=0"
        for /d %%C in ("!decoder_dir!\*") do (
            set "case_dir=%%C"
            set "case_name=%%~nC"

            REM Validate: must have config.json AND input.bin
            if exist "!case_dir!\config.json" if exist "!case_dir!\input.bin" (
                set "found_case=1"

                REM Run the test
                "%DECODER_TEST%" -d !decoder! -t "!case_dir!" !TOLERANCE_ARG! >nul 2>&1
                set /a test_exit=!errorlevel!

                if !test_exit! equ 0 (
                    set /a PASS_COUNT+=1
                    echo   PASS: !decoder!/!case_name!
                ) else if !test_exit! equ 1 (
                    set /a FAIL_COUNT+=1
                    echo   FAIL: !decoder!/!case_name!
                    echo !decoder!/!case_name!>> "!FAILED_FILE!"
                ) else (
                    set /a ERROR_COUNT+=1
                    echo   ERROR: !decoder!/!case_name! ^(exit code !test_exit!^)
                    echo !decoder!/!case_name!: error ^(exit code !test_exit!^)>> "!FAILED_FILE!"
                )
            )
        )

        if "!found_case!"=="0" (
            set /a SKIP_COUNT+=1
            echo !decoder!>> "!SKIPPED_FILE!"
        )
    )
)

set /a TOTAL=PASS_COUNT+FAIL_COUNT+SKIP_COUNT+ERROR_COUNT

REM ---- Print summary ----
echo.
echo ============================================
echo C Decoder Test Results
echo ============================================
echo PASS:  !PASS_COUNT!
echo FAIL:  !FAIL_COUNT!
echo SKIP:  !SKIP_COUNT! (no test data)
echo ERROR: !ERROR_COUNT!
echo TOTAL: !TOTAL!

REM Print failed tests
set "HAS_FAILED=0"
for /f "usebackq" %%l in ("!FAILED_FILE!") do set "HAS_FAILED=1"
if "!HAS_FAILED!"=="1" (
    echo.
    echo Failed tests:
    for /f "usebackq" %%l in ("!FAILED_FILE!") do echo   - %%l
)

REM Print skipped decoders
set "HAS_SKIPPED=0"
for /f "usebackq" %%l in ("!SKIPPED_FILE!") do set "HAS_SKIPPED=1"
if "!HAS_SKIPPED!"=="1" (
    echo.
    echo Skipped ^(need upstream decoder^):
    for /f "usebackq" %%l in ("!SKIPPED_FILE!") do echo   - %%l
)

echo ============================================

REM Cleanup temp files
del "!FAILED_FILE!" 2>nul
del "!SKIPPED_FILE!" 2>nul

REM Exit code
if !FAIL_COUNT! gtr 0 exit /b 1
if !ERROR_COUNT! gtr 0 exit /b 1
exit /b 0
