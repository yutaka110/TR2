"""Determinism, domain separation and frozen-bundle integrity checks."""
import argparse
import copy
import json
from pathlib import Path
from generate_reach_trace import generate, key, sample, encoded, write_bundle, load_bundle, DOMAINS


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=False);checks=[]
    def check(ok,name):
        checks.append(dict(name=name,passed=bool(ok)))
        if not ok:raise AssertionError(name)
    base,meta=generate()
    check(encoded(base)==encoded(generate()[0]),'byte-identical regeneration')
    check(len(set(meta['stream_keys'].values()))==len(DOMAINS),'all named stream keys distinct')
    for domain in DOMAINS:
        changed,other=generate(stream_seeds={domain:999})
        check(all(v==other['stream_keys'][d] for d,v in meta['stream_keys'].items() if d!=domain),'key isolation '+domain)
        if domain in ('scene','perception','policy'):
            check(changed==base,'reserved domain cannot alter links '+domain)
        else:
            _,direction,phenomenon=domain.split('.');opposite='downlink' if direction=='uplink' else 'uplink'
            check(changed[opposite]==base[opposite],'direction isolation '+domain)
            a,b=changed[direction],base[direction]
            if phenomenon=='capacity':check(a['impairment']==b['impairment'],'capacity cannot alter loss/jitter')
            else:
                check(a['capacity']==b['capacity'],'impairment cannot alter capacity '+domain)
                field='drop' if phenomenon=='jitter' else 'delay_us'
                check([x[field] for x in a['impairment']]==[x[field] for x in b['impairment']],'phenomenon isolation '+domain)
                changed_field='delay_us' if phenomenon=='jitter' else 'drop'
                check(any(x[changed_field]!=y[changed_field] for x,y in zip(a['impairment'],b['impairment'])),'changed seed takes effect '+domain)
    for d in DOMAINS:
        for index in reversed(range(2000)):sample(key(201,d),index,1000000)
    check(generate()[0]==base,'unrelated RNG queries cannot consume link sequence')
    shorter,_=generate(horizon_us=6000000)
    check(all(shorter[d][f][:-1]==base[d][f][:60] for d in base for f in ('capacity','impairment')),'horizon extension preserves existing cells')
    folder=args.output/'bundle';write_bundle(folder,horizon_us=6000000)
    check(load_bundle(folder,6000000)[0]==shorter,'bundle replay')
    def reject(action,name):
        try:action()
        except (ValueError,KeyError,TypeError):check(True,name)
        else:check(False,name)
    reject(lambda:load_bundle(folder,6000001),'reject short horizon')
    original=(folder/'link.json').read_bytes();(folder/'link.json').write_bytes(original+b' ')
    reject(lambda:load_bundle(folder,6000000),'reject changed payload bytes');(folder/'link.json').write_bytes(original)
    path=folder/'manifest.json';original=path.read_bytes();changed=json.loads(original);changed['root_seed']+=1;path.write_text(json.dumps(changed))
    reject(lambda:load_bundle(folder,6000000),'reject changed seed without regenerated trace');path.write_bytes(original)
    for args_bad in [dict(seed=-1),dict(seed=True),dict(slot_us=19999),dict(horizon_us=60000001),dict(stream_seeds={'typo':1})]:
        reject(lambda:generate(**args_bad),'invalid generator input '+str(args_bad))
    # Known vector freezes the documented byte encoding, independent of Python RNG.
    import hashlib
    expected=hashlib.sha256(b'Reach-RT/rng/v1\0\x00\x00\x00\xc9link.uplink.loss').digest()
    check(key(201,'link.uplink.loss')==expected,'stream-key byte encoding vector')
    # Different packet/query counts at exact same timestamps return identical states.
    import bisect
    def state(trace,t):return trace[bisect.bisect_right([p['at_us'] for p in trace],t)-1]
    for d in base:
        points=base[d]['impairment'];dense={t:state(points,t) for t in range(0,6000000,1000)}
        sparse={t:state(points,t) for t in range(0,6000000,10000)}
        check(all(dense[t]==v for t,v in sparse.items()),'600 vs 6000 time queries '+d)
    result=dict(passed=all(x['passed'] for x in checks),checks=checks,trace_id=meta['link_sha256'])
    (args.output/'report.json').write_text(json.dumps(result,indent=2));print(json.dumps(dict(passed=result['passed'],checks=len(checks))))


if __name__=='__main__':main()
