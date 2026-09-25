"""Offline exogenous traces. RNG v1 is SHA-256 keyed by domain and time slot.

No packet count, order, task state or policy input enters this generator.
Loss is a time-cell blackout, NOT independent Bernoulli loss per packet.
"""
import argparse
import hashlib
import json
from pathlib import Path

ALGORITHM = 'reach-time-trace-sha256-v1'
DOMAINS = ['scene', 'perception', 'policy'] + [f'link.{d}.{p}' for d in ('uplink', 'downlink') for p in ('capacity', 'loss', 'jitter')]
DEFAULT_PROFILE = {
    'uplink': dict(queue_ip_bytes=65536, capacities_bps=[750000,1500000,3000000,6000000], drop_ppm=100000, base_delay_us=5000, jitter_us=15000),
    'downlink': dict(queue_ip_bytes=16384, capacities_bps=[500000], drop_ppm=100000, base_delay_us=5000, jitter_us=15000),
}


def encoded(value):
    return json.dumps(value, ensure_ascii=True, separators=(',', ':')).encode('ascii')


def key(seed, domain):
    if type(seed) is not int or not 0 <= seed <= 0xffffffff or domain not in DOMAINS:
        raise ValueError('invalid seed/domain')
    return hashlib.sha256(b'Reach-RT/rng/v1\0' + seed.to_bytes(4, 'big') + domain.encode('ascii')).digest()


def sample(stream_key, index, bound):
    # Fixed integer scaling; no platform-dependent random library or shared cursor.
    return (int.from_bytes(hashlib.sha256(stream_key + index.to_bytes(8, 'big')).digest(), 'big') * bound) >> 256


def generate(seed=201, horizon_us=60000000, slot_us=100000, profile=None, stream_seeds=None):
    profile = json.loads(json.dumps(DEFAULT_PROFILE if profile is None else profile))
    stream_seeds = dict(stream_seeds or {})
    if any(d not in DOMAINS for d in stream_seeds): raise ValueError('unknown stream')
    if type(horizon_us) is not int or not 100000 <= horizon_us <= 60000000: raise ValueError('invalid horizon')
    if type(slot_us) is not int or not 20000 <= slot_us <= 1000000: raise ValueError('invalid slot')
    keys = {d:key(stream_seeds.get(d, seed), d) for d in DOMAINS}
    if set(profile) != {'uplink','downlink'}: raise ValueError('profile directions')
    link = {}
    for direction, p in profile.items():
        if set(p) != set(DEFAULT_PROFILE[direction]): raise ValueError('profile keys')
        rates = p['capacities_bps']
        if not isinstance(rates,list) or not 1 <= len(rates) <= 128 or any(type(v) is not int or not 0 <= v <= 1000000000 for v in rates): raise ValueError('profile capacities')
        for name,lo,hi in [('queue_ip_bytes',28,16777216),('drop_ppm',0,1000000),('base_delay_us',0,1000000),('jitter_us',0,1000000)]:
            if type(p[name]) is not int or not lo <= p[name] <= hi: raise ValueError('profile '+name)
        if p['base_delay_us']+p['jitter_us']>1000000: raise ValueError('total delay exceeds one second')
        capacity, impairment = [], []
        for index, at in enumerate(range(0,horizon_us,slot_us)):
            capacity.append(dict(at_us=at,bps=rates[sample(keys[f'link.{direction}.capacity'],index,len(rates))]))
            impairment.append(dict(at_us=at,drop=sample(keys[f'link.{direction}.loss'],index,1000000)<p['drop_ppm'],
                delay_us=p['base_delay_us']+sample(keys[f'link.{direction}.jitter'],index,p['jitter_us']+1)))
        # Explicit normal tail for the bounded shutdown drain. It is also hashed.
        capacity.append(dict(at_us=horizon_us,bps=rates[-1]))
        impairment.append(dict(at_us=horizon_us,drop=False,delay_us=p['base_delay_us']))
        link[direction]=dict(queue_ip_bytes=p['queue_ip_bytes'],capacity=capacity,impairment=impairment)
    metadata=dict(algorithm=ALGORITHM,root_seed=seed,horizon_us=horizon_us,slot_us=slot_us,profile=profile,stream_seeds=stream_seeds,
        stream_keys={d:k.hex() for d,k in keys.items()},reserved_streams=['scene','perception','policy'],
        sample_clock='serialization_completion_us',loss_semantics='time_cell_blackout',intervals='left_closed_right_open; normal tail held',
        link_sha256=hashlib.sha256(encoded(link)).hexdigest())
    return link, metadata


def write_bundle(folder, **kwargs):
    link, metadata = generate(**kwargs)
    folder=Path(folder);folder.mkdir(parents=True,exist_ok=False)
    (folder/'link.json').write_bytes(encoded(link))
    metadata['generator_sha256']=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    (folder/'manifest.json').write_text(json.dumps(metadata,indent=2),encoding='utf-8')
    return link,metadata


def load_bundle(folder, duration_us):
    folder=Path(folder);meta=json.loads((folder/'manifest.json').read_text(encoding='utf-8'))
    link,expected=generate(meta['root_seed'],meta['horizon_us'],meta['slot_us'],meta['profile'],meta['stream_seeds'])
    if set(meta)!=set(expected)|{'generator_sha256'} or any(meta[k]!=v for k,v in expected.items()): raise ValueError('trace manifest is inconsistent')
    if (folder/'link.json').read_bytes()!=encoded(link): raise ValueError('trace payload differs from seed/profile or hash')
    if duration_us>meta['horizon_us']: raise ValueError('trial exceeds frozen trace horizon')
    return link,meta


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path)
    p.add_argument('--seed',type=int,default=201);p.add_argument('--horizon-us',type=int,default=60000000);p.add_argument('--slot-us',type=int,default=100000)
    args=p.parse_args();_,meta=write_bundle(args.output,seed=args.seed,horizon_us=args.horizon_us,slot_us=args.slot_us)
    print(json.dumps(dict(bundle=str(args.output),trace_id=meta['link_sha256'],algorithm=ALGORITHM)))


if __name__=='__main__':main()
