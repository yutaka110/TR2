"""Finite IP-chain POMDP. Exact rationals; no live engine, random sampling or truth policy input.

At a boundary: receive due reports -> choose -> transmit -> decode -> advance task
-> generate a report -> transition hidden channel. One captured frame per slot.
"""
from collections import defaultdict
from dataclasses import dataclass
from fractions import Fraction as F
from functools import lru_cache
from itertools import product

PACKET_IP = 1272  # 1200 payload + 44 RNVP-like + 8 UDP + 20 IPv4
REPORT_IP = 64    # deliberately abstract fixed notification, NOT live RSTA's size
ACTIONS = ('WAIT', 'P', 'P_REPEAT', 'I')
PACKETS = (0, 1, 2, 2)
MODES = ('B1', 'B2', 'B3', 'JOINT')


@dataclass(frozen=True)
class Model:
    horizon: int = 3
    budget_packets: int = 3
    delay: int = 0
    report_loss: F = F(0)
    stay: F = F(3, 4)
    good: F = F(3, 4)
    bad: F = F(1, 4)
    initial: tuple = ((0, 0, 0, F(1, 8)), (0, 0, 1, F(1, 8)),
                      (0, 1, 0, F(1, 8)), (0, 1, 1, F(1, 8)),
                      (1, 0, 0, F(1, 8)), (1, 0, 1, F(1, 8)),
                      (1, 1, 0, F(1, 8)), (1, 1, 1, F(1, 8)))

    def __post_init__(self):
        if any(type(v) is not int for v in (self.horizon,self.budget_packets,self.delay)):
            raise ValueError('horizon, budget and delay must be integers')
        if not (1 <= self.horizon <= 4 and 0 <= self.budget_packets <= 8 and 0 <= self.delay <= 5):
            raise ValueError('model outside declared finite bounds')
        if any(not isinstance(p, F) or not 0 <= p <= 1 for p in (self.report_loss, self.stay, self.good, self.bad)):
            raise ValueError('probabilities must be exact Fractions in [0,1]')
        if sum(p for _, _, _, p in self.initial) != 1 or any(r not in (0, 1) or s not in (0, 1, 2) or c not in (0, 1) or not isinstance(p,F) or p <= 0 for r,s,c,p in self.initial):
            raise ValueError('invalid initial distribution')


def packed(weights):
    return tuple(sorted((s, p) for s, p in weights.items() if p))


def delivery(ref, action, tape):
    if action == 0: return False
    if action == 3: return all(tape)
    return bool(ref and (tape[0] if action == 1 else any(tape)))


class Exact:
    def __init__(self, model, mode='JOINT', policy='exact', node_limit=200000):
        if mode not in MODES or policy not in ('exact', 'greedy'):
            raise ValueError('unknown information mask or policy')
        self.m, self.mode, self.policy, self.limit = model, mode, policy, node_limit
        self.nodes = 0
        self.root_decisions = []
        self.solve = lru_cache(None)(self._solve)
        self.step = lru_cache(None)(self._step)

    def reports(self, ref, stage, pending, stamp, ack):
        due = stamp + self.m.delay
        if due >= self.m.horizon: return ((pending, F(1)),)
        msg = (due, stamp, ack, ref, stage)
        return tuple((q, p) for q, p in ((pending+(msg,), 1-self.m.report_loss), (pending, self.m.report_loss)) if p)

    def initial(self):
        out = defaultdict(F)
        for r,s,c,p in self.m.initial:
            for queue, q in self.reports(r,s,(),0,()): out[(r,s,c,queue)] += p*q
        return packed(out)

    def observations(self, belief, time):
        bins = defaultdict(lambda: defaultdict(F))
        for (ref,stage,channel,pending), prob in belief:
            obs = tuple((stamp, ack, r if self.mode in ('B2','JOINT') else -1,
                         s if self.mode in ('B3','JOINT') else -1)
                        for due,stamp,ack,r,s in pending if due == time)
            rest = tuple(msg for msg in pending if msg[0] > time)
            assert all(msg[0] >= time for msg in pending), 'unconsumed old report'
            bins[obs][(ref,stage,channel,rest)] += prob
        for obs, weights in sorted(bins.items()):
            mass = sum(weights.values())
            yield obs, mass, packed({s:p/mass for s,p in weights.items()})

    def _step(self, time, state, action):
        ref,stage,channel,pending = state
        prob = self.m.good if channel == 0 else self.m.bad
        out = defaultdict(F)
        for tape in product((0,1), repeat=2):
            pt = prob**sum(tape)*(1-prob)**(2-sum(tape))
            if not pt: continue
            decoded = int(delivery(ref,action,tape))
            next_stage = min(2,stage+decoded)
            ack = tape[:PACKETS[action]]  # never reveal unused potential packets
            for queue, pr in self.reports(decoded,next_stage,pending,time+1,ack):
                for next_channel in (0,1):
                    pc = self.m.stay if next_channel == channel else 1-self.m.stay
                    out[(decoded,next_stage,next_channel,queue)] += pt*pr*pc
        assert sum(out.values()) == 1
        return packed(out)

    def transition(self, time, belief, action):
        out = defaultdict(F)
        for state, p in belief:
            for next_state,q in self.step(time,state,action): out[next_state] += p*q
        return packed(out)

    def _solve(self, time, budget, belief):
        self.nodes += 1
        if self.nodes > self.limit: raise RuntimeError('exact node limit exceeded; no approximate result substituted')
        if time == self.m.horizon:
            return sum(p for (_,s,_,_),p in belief if s == 2), F(0)
        total_success, total_cost = F(0),F(0)
        for obs, mass, posterior in self.observations(belief,time):
            candidates = []
            for action, cost in enumerate(PACKETS):
                if cost > budget: continue
                future = self.transition(time,posterior,action)
                value, spent = self.solve(time+1,budget-cost,future)
                progress = sum(p*s for (_,s,_,_),p in future)-sum(p*s for (_,s,_,_),p in posterior)
                score = (value, -spent-cost, -action) if self.policy == 'exact' else (progress-F(1,10)*cost, -cost, -action)
                candidates.append((score,action,value,spent+cost))
            _, action, value, spent = max(candidates)
            if time == 0:
                self.root_decisions.append(dict(observation=obs,mass=mass,action=ACTIONS[action],success=value,
                    alternatives=[dict(action=ACTIONS[a],success=v,expected_packets=c) for _,a,v,c in candidates]))
            total_success += mass*value
            total_cost += mass*spent
        return total_success,total_cost

    def run(self):
        success,cost = self.solve(0,self.m.budget_packets,self.initial())
        return dict(success=success,expected_ip_bytes=cost*PACKET_IP+(self.m.horizon+1)*REPORT_IP,
                    nodes=self.nodes,cache_hits=self.solve.cache_info().hits,root=self.root_decisions)


def channel_paths(model, channel, time=0):
    """Exogenous potential outcomes: same two packet positions for EVERY action."""
    if time == model.horizon: yield (),F(1); return
    p = model.good if channel == 0 else model.bad
    for tape in product((0,1),repeat=2):
        weight = p**sum(tape)*(1-p)**(2-sum(tape))
        if not weight: continue
        if time+1 == model.horizon: yield (tape,),weight; continue
        for nxt in (0,1):
            q = model.stay if nxt == channel else 1-model.stay
            if q:
                for rest,pr in channel_paths(model,nxt,time+1): yield (tape,)+rest,weight*q*pr


def hindsight(model):
    """E[max(policy knowing entire path)] -- never an online comparator."""
    total = F(0); paths = 0
    for ref,stage,channel,prior in model.initial:
        for path,p in channel_paths(model,channel):
            @lru_cache(None)
            def best(t,r,s,b):
                if t == model.horizon: return int(s == 2)
                return max(best(t+1,int(delivery(r,a,path[t])),min(2,s+delivery(r,a,path[t])),b-k)
                           for a,k in enumerate(PACKETS) if k <= b)
            total += prior*p*best(0,ref,stage,model.budget_packets); paths += 1
    return dict(success=total,paths=paths)


def full_state_causal(model):
    @lru_cache(None)
    def best(t,r,s,c,b):
        if t == model.horizon: return F(s == 2)
        p = model.good if c == 0 else model.bad
        values = []
        for a,k in enumerate(PACKETS):
            if k > b: continue
            v = F(0)
            for tape in product((0,1),repeat=2):
                weight = p**sum(tape)*(1-p)**(2-sum(tape))
                dec = int(delivery(r,a,tape))
                for nxt in (0,1):
                    v += weight*(model.stay if nxt == c else 1-model.stay)*best(t+1,dec,min(2,s+dec),nxt,b-k)
            values.append(v)
        return max(values)
    return sum(p*best(0,r,s,c,model.budget_packets) for r,s,c,p in model.initial)
