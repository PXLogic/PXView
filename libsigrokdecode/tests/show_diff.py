import json, sys, os
sys.path.insert(0, os.path.dirname(__file__))
from run_all_tests import run_decoder_test, c_decoder_to_py, compare_annotations

decoder = sys.argv[1] if len(sys.argv) > 1 else 'parallel_c'
testdata_dir = os.path.join('testdata', decoder, 'default')
if not os.path.isdir(testdata_dir):
    testdata_dir = os.path.join('testdata', decoder, '0')

py_data, py_err = run_decoder_test(c_decoder_to_py(decoder), testdata_dir, python_mode=True)
c_data, c_err = run_decoder_test(decoder, testdata_dir, python_mode=False)

if py_err:
    print(f"Py error: {py_err}")
if c_err:
    print(f"C error: {c_err}")

# Show all annotations
print(f"\n=== Python annotations ({len(py_data.get('annotations', []))}) ===")
for a in py_data.get('annotations', []):
    s = a.get('start_sample', 0)
    e = a.get('end_sample', 0)
    cl = a.get('ann_class', 0)
    t = a.get('texts', [])
    print(f'  sample={s}-{e} cls={cl} texts={t}')

print(f"\n=== C annotations ({len(c_data.get('annotations', []))}) ===")
for a in c_data.get('annotations', []):
    s = a.get('start_sample', 0)
    e = a.get('end_sample', 0)
    cl = a.get('ann_class', 0)
    t = a.get('texts', [])
    print(f'  sample={s}-{e} cls={cl} texts={t}')

passed, detail = compare_annotations(py_data, c_data)
print(f"\nResult: {'PASS' if passed else 'FAIL'}")
print(f"Detail: {detail}")
