"""Read-only frozen-source/evidence checks, then recompute the rational study."""
import argparse
from fractions import Fraction as F
import json
from pathlib import Path
import re

from run_reach_h1_study import boundary_table, calculate, encode, histories, plan, sha


def load(p): return json.loads(p.read_text(encoding='utf-8'))
def serial(x): return json.loads(json.dumps(x, default=encode))


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--name', default='study_01')
    a = p.parse_args()
    if not a.name.replace('_', '').isalnum(): raise ValueError('invalid study name')
    repo = Path(__file__).resolve().parents[1]
    out = repo/'artifacts/reach_g3_h1_20260924'/a.name
    manifest = load(out/'evidence.json')['files']
    for f, h in manifest.items():
        path = (out/f).resolve()
        assert path.is_relative_to(out.resolve()) and sha(path) == h, f
    sources = load(out/'source_hashes.json')
    assert all(sha(repo/f) == h and sha(out/'source'/f) == h for f, h in sources.items())
    original = load(out/'plan.json')
    assert original['cases'] == serial(plan())
    report = load(out/'report.json')
    assert report['passed'] and report['target']['exact'] == str(F(19, 20))
    rows = calculate(plan())
    assert report['cases'] == serial(rows), 'rational results or policy changed'
    assert load(out/'histories.json') == histories()
    assert load(out/'boundaries.json') == serial(boundary_table(rows))
    assert load(out/'task_decision.json')['passed']
    links = re.findall(r'href="([^"]+)"', (out/'index.html').read_text(encoding='utf-8'))
    assert all((out/f).is_file() for f in links), 'broken report link'
    print(json.dumps(dict(passed=True, files_checked=len(manifest), sources_checked=len(sources),
                          reproduced_conditions=len(rows), html_links_checked=len(links))))


if __name__ == '__main__': main()
