"""Verify frozen evidence, rational results and a second execution's reproducibility."""
import argparse,hashlib,json
from pathlib import Path


def load(p):return json.loads(p.read_text(encoding='utf-8'))
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def logical(obj):
    if isinstance(obj,dict):return {k:logical(v) for k,v in obj.items() if k!='wall_seconds'}
    if isinstance(obj,list):return [logical(v) for v in obj]
    return obj


def main():
    p=argparse.ArgumentParser();p.add_argument('--name',default='study_03');p.add_argument('--compare',default='study_02');a=p.parse_args()
    repo=Path(__file__).resolve().parents[1];root=repo/'artifacts/reach_g3_exact_20260924';out=root/a.name
    for name in (a.name,a.compare):
        if not name.replace('_','').isalnum():raise ValueError('invalid artifact name')
    manifest=load(out/'evidence.json')['files']
    assert all(sha(out/f)==h for f,h in manifest.items()),'evidence changed'
    source=load(out/'source_hashes.json')
    assert all(sha(repo/f)==h and sha(out/'source'/f)==h for f,h in source.items()),'source changed'
    report=load(out/'report.json');prior=load(root/a.compare/'report.json')
    assert report['passed'] and prior['passed'] and len(report['cases'])==165
    assert logical(report['cases'])==logical(prior['cases']),'exact result is not reproducible'
    for name in ('transport.json','hairpin.json'):
        assert load(out/name)==load(root/a.compare/name),'diagnostic changed'
    assert load(out/'task_decision.json')['passed'] and report['live_engine_unchanged']
    result=dict(passed=True,files_checked=len(manifest),sources_checked=len(source),exact_cases_reproduced=165,
                compared=a.compare,scope='rational outputs, native probe outputs and masks identical; wall timings excluded')
    verifier=Path(__file__).resolve()
    (out/'reproduction_source.py').write_bytes(verifier.read_bytes())
    result['verifier_sha256']=sha(verifier)
    (out/'reproduction.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps(result))


if __name__=='__main__':main()
