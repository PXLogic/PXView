import json, sys

name = sys.argv[1]
start = int(sys.argv[2]) if len(sys.argv) > 2 else 0
end = int(sys.argv[3]) if len(sys.argv) > 3 else 0

fname = 'actual_c' if sys.argv[4] == 'c' else 'expected_py'
with open(f'testdata/{name}_c/default/{fname}.json') as f:
    data = json.load(f)

anns = data['annotations']
if end == 0:
    end = len(anns)
for i in range(start, min(end, len(anns))):
    a = anns[i]
    print(f'[{i}] ss={a["start_sample"]}, es={a["end_sample"]}, cls={a["ann_class"]}, texts={a["texts"]}')
