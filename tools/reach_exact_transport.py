"""Packet-mask enumeration, native 13-plan probes, and Hairpin equations 6/8-11 checks.
All exact claims here are about the declared finite packet models, not Media Foundation.
"""
from collections import defaultdict
from fractions import Fraction as F
from functools import lru_cache
from itertools import product
from math import comb
import json,subprocess


def masks(n,p):
    for bits in product((0,1),repeat=n):
        weight=p**(n-sum(bits))*(1-p)**sum(bits)
        if weight:yield bits,weight  # 1 = received


def hairpin_formula(n,k,p):
    out=[F(0)]*(n+1)
    for lost in range(n+k+1):
        bi=F(comb(n+k,lost))*p**lost*(1-p)**(n+k-lost)
        if lost<=k:out[0]+=bi
        else:
            for missing in range(1,n+1):
                if 0<=lost-missing<=k:
                    out[missing]+=bi*F(comb(n,missing)*comb(k,lost-missing),comb(n+k,lost))
    return tuple(out)


def hairpin_masks(n,k,p):
    out=[F(0)]*(n+1)
    for bits,q in masks(n+k,p):
        missing=0 if sum(bits)>=n else n-sum(bits[:n])
        out[missing]+=q
    return tuple(out)


def hairpin_checks():
    rows=[]
    for n,k,p in product(range(1,5),range(3),(F(0),F(1,10),F(1,4),F(1,2),F(1))):
        analytic=hairpin_formula(n,k,p);enumerated=hairpin_masks(n,k,p)
        assert analytic==enumerated and sum(analytic)==1
        rows.append(dict(data=n,parity=k,loss=p,probabilities=analytic))
    recurrences=[]
    for n,rounds,p in product(range(1,4),range(1,4),(F(1,4),F(1,2))):
        def solve(transition):
            @lru_cache(None)
            def rec(d,left):
                if not d:return (F(0),F(0))
                if not left:return (F(1),F(0))
                choices=[]
                for k in range(3):
                    miss=F(0);packets=F(d+k)
                    for remaining,q in enumerate(transition(d,k,p)):
                        m,c=rec(remaining,left-1);miss+=q*m;packets+=q*c
                    choices.append((miss+F(1,10)*packets,miss,packets,k))
                _,miss,packets,_=min(choices)
                return miss,packets
            return rec(n,rounds)
        a,b=solve(hairpin_formula),solve(hairpin_masks);assert a==b
        recurrences.append(dict(data=n,opportunities=rounds,loss=p,miss=a[0],packets=a[1]))
    assert hairpin_formula(2,1,F(1,4))==(F(27,32),F(3,32),F(1,16))
    return dict(passed=True,transition_cases=rows,recurrence_cases=recurrences,
        scope='Paper equations 6 and 8-11 in a finite ideal-MDS block, plus local 0..2 parity optimization; not full Hairpin implementation, Eq.7 normalized BWC, or block-size optimizer',
        source='https://www.usenix.org/system/files/nsdi24-meng.pdf#page=17')


@lru_cache(None)
def xor_batches(n,group,rounds,p):
    """(round completion index, cumulative video IP bytes, decoded) -> probability.
    Initial parity only; perfect immediate selective ACK, cached parity, no new
    parity on retries. Finish time is tested separately at atomic batch boundaries.
    """
    groups=[tuple(range(i,min(i+group,n))) for i in range(0,n,group)] if group else []
    groups=[g for g in groups if len(g)>1]
    def repair(missing,parities):
        for j,g in enumerate(groups):
            lost=[i for i in g if missing>>i&1]
            if len(lost)==1 and parities>>j&1:missing&=~(1<<lost[0])
        return missing
    active=defaultdict(F);finished=defaultdict(F)
    cost=n*1272+len(groups)*1280
    for bits,q in masks(n+len(groups),p):
        missing=sum((1-bits[i])<<i for i in range(n))
        parity=sum(bits[n+j]<<j for j in range(len(groups)))
        missing=repair(missing,parity)
        if missing:active[(missing,parity,cost)]+=q
        else:finished[(0,cost,True)]+=q
    for r in range(1,rounds+1):
        nxt=defaultdict(F)
        for (missing,parity,spent),prob in active.items():
            indices=[i for i in range(n) if missing>>i&1]
            for bits,q in masks(len(indices),p):
                rest=sum((1-bit)<<i for i,bit in zip(indices,bits))
                rest=repair(rest,parity);total=spent+len(indices)*1272
                if rest:nxt[(rest,parity,total)]+=prob*q
                else:finished[(r,total,True)]+=prob*q
        active=nxt
    for (_,_,spent),prob in active.items():finished[(rounds,spent,False)]+=prob
    assert sum(finished.values())==1
    return tuple(finished.items())


def exact_candidate(n,group,rounds,p,rate,rtt,age,queue):
    success=F(0);expected=F(0)
    for (r,ip,decoded),q in xor_batches(n,group,rounds,p):
        finish=F(queue+rtt//2+8000+r*rtt)+F(ip*8000000,rate)
        if decoded and finish<=200000-age:success+=q
        expected+=q*ip
    return success,expected


def native_checks(exe):
    # Declared grid, including plentiful and tight deadlines. No tuning/selection.
    cases=[(n,p,age) for n,p,age in product((2,3,4),(F(1,10),F(1,4),F(1,2)),(0,60000,80000,100000,120000,150000,180000))]
    inputs=[f'{n*1200} {float(p)} {float(p)} 0.1 3500000 40000 {age} 0 1' for n,p,age in cases]
    # Exercise the ACTUAL native header at several scalar values, not a port.
    equivalence=[]
    for value,weight in product((.15,.5,1.,2.,4.),(0.,.1,.3)):
        if weight/value>3:continue
        equivalence.append((len(inputs),value,weight))
        inputs.extend((f'3600 0.25 0.25 {weight} 3500000 40000 0 0 {value}',
                       f'3600 0.25 0.25 {weight/value} 3500000 40000 0 0 1'))
    correlated=[]
    for n,p in product((2,3,4),(F(1,10),F(1,4),F(1,2))):
        correlated.append((len(inputs),n,p))
        inputs.append(f'{n*1200} {float(p)} 1 0.1 3500000 40000 0 0 1')
    proc=subprocess.run([str(exe)],input='\n'.join(inputs)+'\n',text=True,capture_output=True,check=True)
    outputs=[json.loads(s) for s in proc.stdout.splitlines()];assert len(outputs)==len(inputs)
    rows=[];iid_equal=0
    for (n,p,age),native in zip(cases,outputs):
        exact=[];comparisons=[]
        norm=n*1272+(n//2)*1280
        for c in native['candidates'][:12]:
            s,cost=exact_candidate(n,c['group'],c['rounds'],p,3500000,40000,age,0)
            exact.append(1-s+F(1,10)*cost/norm)
            comparisons.append(dict(group=c['group'],rounds=c['rounds'],native_success=c['success'],exact_success=s,
                native_expected_ip=c['expected_ip'],exact_expected_ip=cost,native_feasible=c['feasible']))
            if age==0:
                assert abs(float(s)-c['success'])<1e-12
                iid_equal+=1
        exact.append(F(1));selected=native['selected'];best=min(range(13),key=lambda i:exact[i])
        rows.append(dict(chunks=n,loss=p,age_us=age,native_selected=selected,exact_selected=best,
            objective_gap=exact[selected]-exact[best],candidates=comparisons))
    for i,v,w in equivalence:assert outputs[i]['selected']==outputs[i+1]['selected']
    burst_rows=[]
    for i,n,p in correlated:
        burst_rows.append(dict(chunks=n,loss=p,burst_input=1,exact_success=1-p,
            candidates=outputs[i]['candidates'][:12],
            scope='One Bernoulli failure coin shared by every packet of the AU and all retries. Within-block conditional loss=1; no EWMA trace replay. Generous deadline.'))
    return dict(passed=True,grid=rows,iid_probability_equal_checks=iid_equal,scalar_equivalence_checks=len(equivalence),
        correlated_cases=burst_rows,native_input_lines=inputs,native_output=outputs,
        scope='Same 13 choices; one-AU ideal selective retry + cached XOR + atomic batch timing, unlimited 65536-byte cap, video-only expected bytes; not RNVP wire execution or task regret')
