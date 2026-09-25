"""G4-04 restriction of the LIVE short-horizon rank, not a live RNVP simulator.

Same G3 finite state, observations, actions and budget for causal optimum/proxy.
One-step probability is exact under the available belief: prediction error is
removed to expose horizon/objective mismatch. No future trace enters a policy.
"""
from collections import defaultdict
from fractions import Fraction as F
from functools import lru_cache
from reach_exact_model import Exact,PACKETS,PACKET_IP,ACTIONS

class RestrictedScheduler(Exact):
    def __init__(self,model,mode='JOINT',weight=F(1,10)):
        super().__init__(model,mode);self.weight=weight;self.optimum=Exact(model,mode);self.ranks={};self.visits=[]
    def _solve(self,time,budget,belief):
        self.nodes+=1
        if self.nodes>self.limit:raise RuntimeError('restricted node limit exceeded')
        if time==self.m.horizon:return sum(p for (_,s,_,_),p in belief if s==2),F(0)
        success=spent=F(0)
        for obs,mass,posterior in self.observations(belief,time):
            alternatives=[]
            for action,cost in enumerate(PACKETS):
                if cost>budget:continue
                future=self.transition(time,posterior,action)
                immediate=sum(p*s for (_,s,_,_),p in future)-sum(p*s for (_,s,_,_),p in posterior)
                score=float(immediate)-float(self.weight)*cost*PACKET_IP/65536
                value,_=self.optimum.solve(time+1,budget-cost,future)
                alternatives.append(dict(action=action,cost=cost,immediate=immediate,score=score,optimal_value=value,future=future))
            chosen=max(alternatives,key=lambda a:(a['score'],-a['action']))
            signature=tuple((str(a['immediate']),a['cost']*PACKET_IP) for a in alternatives)
            self.ranks[(str(self.weight),signature)]=alternatives.index(chosen)
            best=max(a['optimal_value'] for a in alternatives);optimal=[a['action'] for a in alternatives if a['optimal_value']==best]
            self.visits.append(dict(time=time,budget=budget,observation=obs,selected=ACTIONS[chosen['action']],optimal_actions=[ACTIONS[a] for a in optimal],
                non_tie_mismatch=chosen['action'] not in optimal,one_step_regret=best-chosen['optimal_value'],
                alternatives=[{k:v for k,v in a.items() if k!='future'} for a in alternatives]))
            value,cost=self.solve(time+1,budget-chosen['cost'],chosen['future']);success+=mass*value;spent+=mass*(cost+chosen['cost'])
        return success,spent

def independent_proxy(model,mode):
    """Independent world-path enumeration and observation grouping (horizon <=2)."""
    from itertools import product
    worlds=[]
    for ref,stage,channel,prior in model.initial:
        for tail in product((0,1),repeat=model.horizon-1):
            channels=(channel,)+tail;cp=prior
            for i in range(1,len(channels)):cp*=model.stay if channels[i]==channels[i-1] else 1-model.stay
            for bits in product((0,1),repeat=2*model.horizon):
                bp=cp
                for t in range(model.horizon):
                    q=model.good if channels[t]==0 else model.bad
                    for bit in bits[2*t:2*t+2]:bp*=q if bit else 1-q
                for keeps in product((0,1),repeat=model.horizon):
                    prob=bp
                    for keep in keeps:prob*=1-model.report_loss if keep else model.report_loss
                    if prob:worlds.append((ref,stage,bits,keeps,((0,(),ref,stage),),prob))
    def step(t,budget,ws):
        if t==model.horizon:return sum(w[-1] for w in ws if w[1]==2)
        groups=defaultdict(list)
        for w in ws:
            r,s,bits,keeps,msgs,p=w;stamp=t-model.delay;obs=()
            if stamp>=0 and keeps[stamp]:
                gen,ack,rr,ss=msgs[stamp];obs=(gen,ack,rr if mode in ('B2','JOINT') else -1,ss if mode in ('B3','JOINT') else -1)
            groups[obs].append(w)
        value=F(0)
        for ws in groups.values():
            mass=sum(w[-1] for w in ws);choices=[]
            for a,k in enumerate((0,1,2,2)):
                if k>budget:continue
                nxt=[];progress=F(0)
                for r,s,bits,keeps,msgs,p in ws:
                    x,y=bits[2*t:2*t+2];decoded=int(bool((a==3 and x and y) or (r and ((a==1 and x) or (a==2 and (x or y))))))
                    ns=min(2,s+decoded);progress+=p*(ns-s)
                    nxt.append((decoded,ns,bits,keeps,msgs+((t+1,bits[2*t:2*t+k],decoded,ns),),p))
                score=float(progress/mass)-.1*k*1272/65536;choices.append((score,-a,k,nxt))
            _,_,k,nxt=max(choices,key=lambda c:c[:2]);value+=step(t+1,budget-k,nxt)
        return value
    return step(0,model.budget_packets,worlds)
