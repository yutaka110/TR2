"""Independent world enumeration and analytic checks for G3-04."""
from collections import defaultdict
from dataclasses import replace
from fractions import Fraction as F
from itertools import product
import unittest

from reach_h1_model import H1Model, H1Exact, evaluate, histories


def exhaustive(m, mode):
    """Enumerate all potential packet/reset/report outcomes BEFORE any decisions.

    Does not call the solver's transition, projection or delivery functions.
    Hidden future coins stay in worlds; decisions group only arrived reports.
    """
    h = m.horizon
    worlds = []
    for r, initial_prob in ((0, 1-m.ref_prior), (1, m.ref_prior)):
        if not initial_prob: continue
        for bits in product((0, 1), repeat=2*h):
            pb = m.packet_success**sum(bits)*(1-m.packet_success)**(2*h-sum(bits))
            if not pb: continue
            for resets in product((0, 1), repeat=h):
                pr = m.reset**sum(resets)*(1-m.reset)**(h-sum(resets))
                if not pr: continue
                for kept in product((0, 1), repeat=h):
                    pq = (1-m.report_loss)**sum(kept)*m.report_loss**(h-sum(kept))
                    if not pq: continue
                    msgs = ((0, (), m.initial_stage, -m.initial_age, r),) if kept[0] else ()
                    worlds.append((initial_prob*pb*pr*pq, r, m.initial_stage,
                                   -m.initial_age, msgs, bits, resets, kept))

    def best(time, budget, pool):
        if time == h:
            return sum(w[0] for w in pool if w[2] == 2), F(0)
        groups = defaultdict(list)
        for world in pool:
            obs = tuple((stamp, ack, stage, cap, ref if mode == 'REF' else -1)
                        for stamp, ack, stage, cap, ref in world[4] if stamp+m.delay == time)
            groups[obs].append(world)
        val, cost = F(0), F(0)
        for group in groups.values():
            choices = []
            for action, amount in enumerate((0, 1, 2, 2)):
                if amount > budget: continue
                nxt = []
                for prob, ref, stage, cap, msgs, bits, resets, kept in group:
                    x, y = bits[2*time:2*time+2]
                    ok = (action == 3 and x == y == 1) or (ref == 1 and
                          ((action == 1 and x == 1) or (action == 2 and x+y > 0)))
                    stage2 = min(2, stage+int(ok))
                    cap2 = time if ok else cap
                    ref2 = int(ok and resets[time] == 0)
                    if time+1 < h and kept[time+1]:
                        msgs = msgs+((time+1, (x, y)[:amount], stage2, cap2, ref2),)
                    nxt.append((prob, ref2, stage2, cap2, msgs, bits, resets, kept))
                v, c = best(time+1, budget-amount, nxt)
                choices.append((v, c+amount*sum(w[0] for w in group), action))
            v, c, _ = max(choices, key=lambda x: (x[0], -x[1], -x[2]))
            val += v
            cost += c
        return val, cost
    assert sum(w[0] for w in worlds) == 1
    return best(0, m.budget_packets, worlds)


class Checks(unittest.TestCase):
    def test_independent_worlds(self):
        # 2 task stages x 3 delays x 2 losses x 2 flush probabilities x 2 masks.
        for stage, delay, loss, reset in product((0, 1), (0, 1, 2), (F(0), F(1, 2)), (F(0), F(1, 4))):
            m = H1Model(horizon=2, budget_packets=3, initial_stage=stage,
                        delay=delay, report_loss=loss, reset=reset, packet_success=F(3, 4))
            for mode in ('AGE', 'REF'):
                with self.subTest(stage=stage, delay=delay, loss=loss, reset=reset, mode=mode):
                    v, c = exhaustive(m, mode)
                    actual = H1Exact(m, mode).run()
                    self.assertEqual((actual['success'], actual['expected_ip_bytes']), (v, c*1272+192))

    def test_one_slot_formula_and_strict_pair(self):
        for p, prior in product((F(0), F(1, 2), F(3, 4), F(9, 10), F(1)), (F(0), F(1, 4), F(1, 2), F(1))):
            r = evaluate(H1Model(packet_success=p, ref_prior=prior))
            self.assertEqual(r['AGE']['success'], max(p*p, prior*(2*p-p*p)))
            self.assertEqual(r['REF']['success'], (1-prior)*p*p+prior*(2*p-p*p))
        r = evaluate(H1Model())
        self.assertEqual(r['AGE']['success'], F(81, 100))
        self.assertEqual(r['REF']['success'], F(9, 10))
        self.assertEqual([x['optimal_actions'] for x in r['REF']['root']], [['I'], ['P_REPEAT']])

    def test_histories_are_matched(self):
        a, b = histories()
        self.assertEqual(a['common'], b['common'])
        c = a['common']
        self.assertLessEqual(c['capture'], c['decoded'])
        self.assertLessEqual(c['decoded'], c['now'])
        self.assertEqual(c['aoi'], c['now']-c['capture'])
        self.assertTrue(a['controller_image_retained'] and b['controller_image_retained'])

    def test_capture_stamp_and_flush(self):
        solver = H1Exact(H1Model(horizon=2, packet_success=F(1), reset=F(1)))
        states = solver.step(0, (1, 0, -2, ()), 1)
        self.assertEqual(len(states), 1)
        (ref, stage, cap, messages), prob = states[0]
        self.assertEqual((ref, stage, cap, prob), (0, 1, 0, 1))
        self.assertEqual(messages[0], (1, 1, (1,), 1, 0, 0))
        # Buffer flush does not erase a usable output; no new output ages it.
        (r, s, cap, _), _ = solver.step(1, (ref, stage, cap, ()), 0)[0]
        self.assertEqual((r, s, 2-cap), (0, 1, 2))

    def test_no_truth_or_future_notification(self):
        m = H1Model(horizon=2, delay=1)
        for mode in ('AGE', 'REF'):
            solver = H1Exact(m, mode)
            bins = list(solver.observations(solver.initial(), 0))
            self.assertEqual(len(bins), 1)
            self.assertEqual(bins[0][0], ())
            self.assertEqual(len(bins[0][2]), 2)
        solver = H1Exact(replace(m, delay=0), 'AGE')
        self.assertEqual(len(list(solver.observations(solver.initial(), 0))), 1)

    def test_no_value_controls(self):
        for m in (H1Model(budget_packets=0), H1Model(horizon=3, delay=3),
                  H1Model(horizon=3, report_loss=F(1)), H1Model(packet_success=F(1)),
                  H1Model(packet_success=F(0)), H1Model(horizon=3, initial_stage=0, ref_prior=F(1)),
                  H1Model(horizon=3, initial_stage=0, ref_prior=F(0))):
            r = evaluate(m)
            self.assertEqual(r['AGE']['success'], r['REF']['success'])

    def test_region_and_monotonicity(self):
        for prior in (F(0), F(1, 2), F(1)):
            m = H1Model(ref_prior=prior)
            old = F(0)
            for b in range(7):
                value = evaluate(replace(m, horizon=2, budget_packets=b))['REF']['success']
                self.assertGreaterEqual(value, old)
                old = value
            old = F(0)
            for h in range(1, 5):
                value = evaluate(replace(m, horizon=h))['REF']['success']
                self.assertGreaterEqual(value, old)
                old = value
        self.assertLess(evaluate(H1Model(ref_prior=F(0)))['REF']['success'], F(19, 20))
        self.assertGreaterEqual(evaluate(H1Model(ref_prior=F(1)))['REF']['success'], F(19, 20))

    def test_feasibility_expansion_independent(self):
        m = H1Model(horizon=3, budget_packets=5, initial_stage=0)
        for mode, expected in (('AGE', F(94041, 100000)), ('REF', F(9639, 10000))):
            v, cost = exhaustive(m, mode)
            actual = H1Exact(m, mode).run()
            self.assertEqual(v, expected)
            self.assertEqual((v, cost*1272+256), (actual['success'], actual['expected_ip_bytes']))

    def test_validation_and_fail_closed(self):
        for args in (dict(horizon=1.5), dict(delay=True), dict(initial_age=0),
                     dict(budget_packets=9), dict(reset=0.1), dict(ref_prior=F(2))):
            with self.assertRaises(ValueError): H1Model(**args)
        with self.assertRaises(RuntimeError): H1Exact(H1Model(), node_limit=1).run()


if __name__ == '__main__': unittest.main(verbosity=2)
