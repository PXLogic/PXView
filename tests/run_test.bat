@echo off
cd /d "c:\Users\admin\Downloads\Downloads\DSView-main_2026_4_27cppnb\tests"
set PYTHONUNBUFFERED=1
python -u -m pytest suites/ -v -s --tb=short --timeout=120 --timeout-method=thread > test_run3.txt 2>&1
