"""Independent full-world enumeration and invariants for the finite exact solver."""
from collections import defaultdict
from dataclasses import replace
from fractions import Fraction as F
from itertools import product
import unittest
from reach_exact_model import Model,Exact,MODES,PACKET_IP,REPORT_IP,hindsight,full_state_causal


def exhaustive(model, mode):
    # Independently expand all channel histories, packet outcomes and notification
    # delivery coins BEFORE evaluating a shared causal policy tree. No solver helpers.
    worlds=[]
    for r,s,c,prior in model.initial:
        for channels in product((0,1),repeat=model.horizon-1):
            cs=(c,)+channels
            cp=prior
            for i in range(1,len(cs)):cp*=model.stay if cs[i]==cs[i-1] else 1-model.stay
            for bits in product((0,1),repeat=2*model.horizon):
                bp=cp
                for t in range(model.horizon):
                    q=model.good if cs[t]==0 else model.bad
                    for b in bits[2*t:2*t+2]:bp*=q if b else 1-q
                for keeps in product((0,1),repeat=model.horizon):
                    p=bp
                    for keep in keeps:p*=1-model.report_loss if keep else model.report_loss
                    if p:worlds.append((r,s,bits,keeps,((0,(),r,s),),p))
    assert sum(w[-1] for w in worlds)==1
    calls=0
    def tree(t,budget,ws):
        nonlocal calls
        calls+=1
        if t==model.horizon:return (sum(p for r,s,bits,keeps,msgs,p in ws if s==2),F(0))
        groups=defaultdict(list)
        for w in ws:
            r,s,bits,keeps,msgs,p=w
            stamp=t-model.delay;obs=()
            if stamp>=0 and keeps[stamp]:
                gen,ack,oldr,olds=msgs[stamp]
                obs=(gen,ack,oldr if mode in ('B2','JOINT') else -1,olds if mode in ('B3','JOINT') else -1)
            groups[obs].append(w)
        result=[F(0),F(0)]
        for ws in groups.values():
            choices=[]
            for a,k in enumerate((0,1,2,2)):
                if k>budget:continue
                next_worlds=[]
                for r,s,bits,keeps,msgs,p in ws:
                    x,y=bits[2*t:2*t+2]
                    decoded=(a==3 and x and y) or (r and ((a==1 and x) or (a==2 and (x or y))))
                    nr=int(bool(decoded));ns=min(2,s+nr)
                    next_worlds.append((nr,ns,bits,keeps,msgs+((t+1,bits[2*t:2*t+k],nr,ns),),p))
                v,c=tree(t+1,budget-k,next_worlds)
                choices.append((v,-c-k*sum(w[-1] for w in ws)))
            v,negc=max(choices);result[0]+=v;result[1]-=negc
        return tuple(result)
    v,c=tree(0,model.budget_packets,worlds)
    return v,c*PACKET_IP+(model.horizon+1)*REPORT_IP,calls


class ExactTests(unittest.TestCase):
    def test_world_enumeration(self):
        for delay,loss in ((0,F(0)),(0,F(1,2)),(1,F(1,2)),(2,F(0))):
            model=Model(horizon=2,budget_packets=3,delay=delay,report_loss=loss)
            for mode in MODES:
                with self.subTest(delay=delay,loss=loss,mode=mode):
                    got=Exact(model,mode).run();v,c,_=exhaustive(model,mode)
                    self.assertEqual((got['success'],got['expected_ip_bytes']),(v,c))

    def test_information_and_oracle_order(self):
        for delay in (0,1,3):
            m=Model(delay=delay,report_loss=F(1,2))
            v={mode:Exact(m,mode).run()['success'] for mode in MODES}
            self.assertLessEqual(v['B1'],min(v['B2'],v['B3']))
            self.assertLessEqual(max(v['B2'],v['B3']),v['JOINT'])
            self.assertLessEqual(v['JOINT'],full_state_causal(m))
            self.assertLessEqual(full_state_causal(m),hindsight(m)['success'])
            for mode in MODES:self.assertLessEqual(Exact(m,mode,'greedy').run()['success'],v[mode])

    def test_no_arrival_no_value(self):
        for m in (Model(delay=3),Model(report_loss=F(1))):
            results=[Exact(m,mode).run()['success'] for mode in MODES]
            self.assertEqual(len(set(results)),1)

    def test_future_reports_are_not_observations(self):
        b=(((1,0,0,((2,0,(1,),1,0),)),F(1)),)
        for mode in MODES:
            observations=list(Exact(Model(),mode).observations(b,0))
            self.assertEqual(observations[0][0],())
            self.assertEqual(observations[0][2],b)
        self.assertGreater(hindsight(Model())['success'],full_state_causal(Model()))

    def test_transport_golden_cases(self):
        from reach_exact_transport import xor_batches,exact_candidate,hairpin_formula
        p=F(1,4)
        self.assertEqual(sum(q for (r,c,ok),q in xor_batches(2,0,0,p) if ok),F(9,16))
        self.assertEqual(sum(q for (r,c,ok),q in xor_batches(2,2,0,p) if ok),F(27,32))
        self.assertEqual(hairpin_formula(2,1,p),(F(27,32),F(3,32),F(1,16)))
        self.assertEqual(exact_candidate(3,0,2,p,3500000,40000,80000,0)[0],F(3375,4096))

    def test_no_budget_and_deterministic_cases(self):
        self.assertEqual(Exact(Model(budget_packets=0)).run()['success'],0)
        for ref,stage,budget,horizon,expected in ((0,0,2,2,0),(0,0,3,2,1),(1,0,2,2,1),(0,1,1,1,0),(0,1,2,1,1)):
            m=Model(horizon=horizon,budget_packets=budget,good=F(1),bad=F(1),initial=((ref,stage,0,F(1)),))
            self.assertEqual(Exact(m).run()['success'],expected)

    def test_monotone_budget_and_deadline(self):
        for mode in MODES:
            old=F(0)
            for budget in range(6):
                v=Exact(Model(budget_packets=budget),mode).run()['success']
                self.assertGreaterEqual(v,old);old=v
            values=[Exact(Model(horizon=h),mode).run()['success'] for h in (1,2,3)]
            self.assertEqual(values,sorted(values))

    def test_bounds_fail_closed(self):
        for kw in ({'horizon':5},{'horizon':1.5},{'delay':True},{'budget_packets':-1},{'report_loss':.5},{'initial':((0,0,0,F(1,2)),)}):
            with self.assertRaises(ValueError):Model(**kw)
        with self.assertRaises(RuntimeError):Exact(Model(),node_limit=1).run()

    def test_hidden_fields_masked(self):
        # Two hidden worlds with a same-time report must be indistinguishable
        # when only the forbidden field differs. No alias via pending payload.
        for mode,worlds in (('B2',((1,0),(1,1))),('B3',((0,1),(1,1))),('B1',((0,0),(1,1)))):
            b=tuple(((r,s,0,((0,0,(),r,s),)),F(1,2)) for r,s in worlds)
            self.assertEqual(len(list(Exact(Model(),mode).observations(b,0))),1)


if __name__=='__main__':unittest.main(verbosity=2)
