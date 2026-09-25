"""G3-04: finite causal model with explicit capture-age and decoder buffer loss.

AGE observes ACKs, task stage and the capture stamp of the last usable image.
REF additionally observes the decoder's reference availability. Both use the
entire delivered history, equal fixed-size reports, legal actions and budgets.
This is an offline mathematical experiment, not a model of full H.264/robotics.
"""
from collections import defaultdict
from dataclasses import dataclass
from fractions import Fraction as F
from functools import lru_cache
from itertools import product

from reach_exact_model import ACTIONS, PACKETS, PACKET_IP, REPORT_IP


@dataclass(frozen=True)
class H1Model:
    horizon: int = 1
    budget_packets: int = 2
    delay: int = 0
    report_loss: F = F(0)
    packet_success: F = F(9, 10)
    ref_prior: F = F(1, 2)
    reset: F = F(0)
    initial_stage: int = 1
    initial_age: int = 2

    def __post_init__(self):
        ints = (self.horizon, self.budget_packets, self.delay, self.initial_stage, self.initial_age)
        if any(type(x) is not int for x in ints):
            raise ValueError('finite indices must be integers, not floats/bools')
        if not (1 <= self.horizon <= 4 and 0 <= self.budget_packets <= 8
                and 0 <= self.delay <= 5 and self.initial_stage in (0, 1)
                and 1 <= self.initial_age <= 5):
            raise ValueError('outside declared finite bounds')
        if any(not isinstance(p, F) or not 0 <= p <= 1 for p in
               (self.report_loss, self.packet_success, self.ref_prior, self.reset)):
            raise ValueError('probabilities must be Fractions in [0,1]')


def packed(weights):
    return tuple(sorted((s, p) for s, p in weights.items() if p))


def histories(age=2, stage=1):
    """Reachable histories: controller keeps image; decoder alone may be flushed.

    Before the decision, one identical I frame arrives in both worlds. There
    are no intervening captures. Flushing a reference buffer emits no image and
    changes neither the stored capture stamp nor the already completed stage.
    """
    if type(age) is not int or not 1 <= age <= 5 or stage not in (0, 1):
        raise ValueError('invalid history')
    common = dict(now=0, capture=-age, decoded=-age+1, aoi=age,
                  packet_ack=[1, 1], packet_delays=[1, 1], loss=0,
                  task_stage=stage, intervening_captures=0)
    return [dict(common=common.copy(), decoder_reference=r,
                 decoder_event='retain_reference' if r else 'flush_reference_after_output',
                 controller_image_retained=True) for r in (0, 1)]


class H1Exact:
    def __init__(self, model, mode='AGE', node_limit=200000):
        if mode not in ('AGE', 'REF'):
            raise ValueError('unknown information mask')
        self.m, self.mode, self.limit = model, mode, node_limit
        self.nodes, self.root = 0, []
        self.solve = lru_cache(None)(self._solve)
        self.step = lru_cache(None)(self._step)

    def reports(self, r, s, capture, pending, stamp, ack):
        due = stamp + self.m.delay
        if due >= self.m.horizon:
            return ((pending, F(1)),)
        msg = (due, stamp, ack, s, capture, r)
        return tuple((v, p) for v, p in
                     ((pending+(msg,), 1-self.m.report_loss), (pending, self.m.report_loss)) if p)

    def initial(self):
        states = defaultdict(F)
        for r, p in ((0, 1-self.m.ref_prior), (1, self.m.ref_prior)):
            if not p: continue
            for pending, q in self.reports(r, self.m.initial_stage, -self.m.initial_age, (), 0, ()):
                states[(r, self.m.initial_stage, -self.m.initial_age, pending)] += p*q
        return packed(states)

    def observations(self, belief, time):
        bins = defaultdict(lambda: defaultdict(F))
        for (r, s, capture, pending), p in belief:
            assert all(msg[0] >= time for msg in pending)
            obs = tuple((stamp, ack, stage, cap, ref if self.mode == 'REF' else -1)
                        for due, stamp, ack, stage, cap, ref in pending if due == time)
            rest = tuple(msg for msg in pending if msg[0] > time)
            bins[obs][(r, s, capture, rest)] += p
        for obs, weights in sorted(bins.items()):
            mass = sum(weights.values())
            yield obs, mass, packed({s: p/mass for s, p in weights.items()})

    def _step(self, time, state, action):
        r, stage, capture, pending = state
        p = self.m.packet_success
        states = defaultdict(F)
        for bits in product((0, 1), repeat=2):
            weight = p**sum(bits)*(1-p)**(2-sum(bits))
            if not weight: continue
            decoded = bool((action == 3 and all(bits)) or
                           (r and ((action == 1 and bits[0]) or (action == 2 and any(bits)))))
            ns, cap = min(2, stage+decoded), time if decoded else capture
            # A flush occurs AFTER any usable output; it does not erase that output.
            for lost, pr in ((0, 1-self.m.reset), (1, self.m.reset)):
                if not pr: continue
                nr = int(decoded and not lost)
                for queue, pq in self.reports(nr, ns, cap, pending, time+1, bits[:PACKETS[action]]):
                    states[(nr, ns, cap, queue)] += weight*pr*pq
        assert sum(states.values()) == 1
        return packed(states)

    def transition(self, time, belief, action):
        states = defaultdict(F)
        for state, p in belief:
            for nxt, q in self.step(time, state, action): states[nxt] += p*q
        return packed(states)

    def _solve(self, time, budget, belief):
        self.nodes += 1
        if self.nodes > self.limit:
            raise RuntimeError('exact node limit exceeded; no approximation substituted')
        if time == self.m.horizon:
            return sum(p for (_, stage, _, _), p in belief if stage == 2), F(0)
        value, cost = F(0), F(0)
        for obs, mass, posterior in self.observations(belief, time):
            candidates = []
            for a, k in enumerate(PACKETS):
                if k > budget: continue
                v, c = self.solve(time+1, budget-k, self.transition(time, posterior, a))
                candidates.append((v, c+k, a))
            v, c, a = max(candidates, key=lambda x: (x[0], -x[1], -x[2]))
            if time == 0:
                self.root.append(dict(observation=obs, mass=mass, action=ACTIONS[a],
                    success=v, optimal_actions=[ACTIONS[j] for w, _, j in candidates if w == v],
                    alternatives=[dict(action=ACTIONS[j], success=w, expected_packets=z)
                                  for w, z, j in candidates]))
            value += mass*v
            cost += mass*c
        return value, cost

    def run(self):
        v, c = self.solve(0, self.m.budget_packets, self.initial())
        return dict(success=v, expected_ip_bytes=c*PACKET_IP+(self.m.horizon+1)*REPORT_IP,
                    nodes=self.nodes, root=self.root)


def evaluate(model):
    results = {}
    for mode in ('AGE', 'REF'):
        solver = H1Exact(model, mode)
        results[mode] = solver.run()
        solver.solve.cache_clear()
        solver.step.cache_clear()
    assert results['AGE']['success'] <= results['REF']['success'] <= 1
    assert all(x['expected_ip_bytes'] <= model.budget_packets*PACKET_IP+
               (model.horizon+1)*REPORT_IP for x in results.values())
    return results
