# Phase 7 — Risk and recovery: results

The build plan's done-condition, verbatim:

> Take a real trading day and inject: dropped packets, reordered packets,
> duplicated packets, a mid-day disconnect at 14:00, a trading halt. The system
> must either produce correct state or halt safely. **Never silently wrong.**

Everything below is about the last three words. Correct-or-safe is easy to
claim and hard to check, because the failure it excludes is by construction the
one that does not announce itself.

---

## 1. The done-condition

```
python3 python/analysis/adversarial.py data/raw/queue_long.gz --build build
```

Ten scenarios over a 200,056-message feed, wrapped into 4,929 MoldUDP64 packets
and then damaged. Each run is graded on two facts: whether the final book
matches an undamaged replay, and whether the system said it was trustworthy.

| verdict | book | system said | meaning |
|---|---|---|---|
| **CORRECT** | matches | trusted | survived, and knew |
| **SAFE** | differs | not trusted | damaged, and did not pretend otherwise |
| **CAUTIOUS** | matches | not trusted | over-careful; a pass, but counted |
| **WRONG** | differs | trusted | **silently wrong** |

`CAUTIOUS` is counted rather than waved through because a system that always
answers "untrusted" is trivially never wrong and also useless. If that column
fills up, the convergence bar is set too high to be worth anything.

Books are compared **mid-session**, at 90% of the feed's time span — see
section 3, this was the second bug real data found.

| scenario | verdict | lost | gaps | dup | reord | trunc | state |
|---|---|---:|---:|---:|---:|---:|---|
| clean | CORRECT | 0 | 0 | 0 | 0 | 0 | trusted |
| drop-1-in-1000 | SAFE | 199 | 5 | 0 | 320 | 0 | recovering |
| drop-1-in-100 | SAFE | 2,388 | 58 | 0 | 2,263 | 0 | recovering |
| duplicate-1-in-100 | CORRECT | 0 | 0 | 2,388 | 0 | 0 | trusted |
| reorder-1-in-100 | CORRECT | 0 | 0 | 0 | 56 | 0 | trusted |
| truncate-1-in-500 | SAFE | 249 | 12 | 0 | 0 | 12 | recovering |
| disconnect-short | CORRECT | 1,617 | 1 | 0 | 64 | 0 | trusted |
| disconnect-long | CORRECT | 16,231 | 1 | 0 | 64 | 0 | trusted |
| everything | SAFE | 1,942 | 11 | 444 | 461 | 4 | recovering |
| disconnect-to-end | SAFE | 0 | 0 | 0 | 0 | 0 | halted |

**5 CORRECT, 5 SAFE, 0 WRONG.**

The two worth pausing on are the outages. `disconnect-long` loses 16,231
consecutive messages — one hole, no retransmission available — and the book
converges back to **byte-identical** with the truth before the session ends.
`disconnect-to-end` loses nothing by the counters and is still not trusted, for
reasons in section 3.

---

## 2. Proving the harness can fail

A grader that has only ever returned CORRECT is not evidence of anything. The
convergence bar — how many consecutive resolvable order references the book
must see before it calls itself whole again — is the parameter that separates
safe from silently wrong, so setting it to a single reference should break
things, and does:

```
python3 python/analysis/adversarial.py data/raw/queue_long.gz --build build \
    --recover-after 1
```

| bar | outcome |
|---|---|
| a 20,000-reference window | correct or safe in every scenario |
| 1 | **WRONG in four scenarios**, exit 1 |

Both runs are in CI: the first must pass, the second must fail. The number is
not derived from anything. It is the line between those two rows, which is why
`gap_policy.hpp` calls it policy and names it rather than presenting it as a
consequence of the design.

---

## 3. Two bugs, both found by the harness rather than by the code

### A test that did not fire

The first matrix reported CORRECT for both disconnect scenarios. That looked
like a robust system and was a mis-specified test: the outage was pinned to
14:00, the synthetic feed spans two minutes from 09:30, and the scenario never
fired at all. **A test that does not fire is indistinguishable from a test that
passes.**

Fixed twice over. The outage moment is computed from the feed — 14:00 when the
data covers it, 60% through when it does not, and the substitution is printed
rather than silently applied. And every damaging scenario now has to prove it
damaged something before its verdict is allowed to count.

### A stream that stopped rather than ended

Sweeping outage lengths past the point where the feed resumes found this:

| outage (packets) | messages lost | state | book == truth | verdict |
|---:|---:|---|---|---|
| 5 | 202 | trusted | yes | CORRECT |
| 20 | 810 | trusted | yes | CORRECT |
| 40 | 1,617 | trusted | yes | CORRECT |
| 100 | 4,049 | trusted | yes | CORRECT |
| 400 | 16,231 | trusted | yes | CORRECT |
| 1,000 | 40,630 | recovering | no | SAFE |
| 2,000 | **0** | halted | no | SAFE |

The last row is the bug. An outage of 2,000 packets at 60% through runs past
the end of the file, so the feed never comes back — and **before the fix that
row read `0 lost, 0 gaps, trusted`, with 80,235 messages missing. 40% of the
session, gone, reported clean.**

The mechanism is worth stating because it generalises. A gap in the middle
announces itself: the next packet carries a sequence number that has jumped. A
stream that dies never sends that next packet, so there is no discontinuity to
notice and every counter stays honestly at zero. Nothing was detected because
nothing was detectable from sequence numbers alone.

MoldUDP64 has the answer already: an end-of-session marker. A stream that ends
without one did not end, it stopped. That is now `Halted` — not `Recovering`,
because there is no bound on what was missed and no further messages with which
to demonstrate convergence.

The adversarial matrix missed this because every outage in it was short enough
that the feed resumed. `disconnect-to-end` is now a scenario, and two unit
tests pin the rule.

---

## 3b. Two more bugs, both found by real data

Phase 6's lesson, repeating: the synthetic feed agreed with everything, and
MSFT did not.

### The comparison compared nothing

Pointed at a real day the harness printed `truth: 0 levels` and then graded ten
scenarios against it. A real trading day ends with every order cancelled, so
the end-of-day book is empty — and comparing two empty books matches trivially.
Every verdict in that run was unearned.

Books are now sampled mid-session, at 90% of the feed's span: well after the
outage, well before the close. And a run whose checkpoint book is empty fails
outright rather than passing vacuously, on the same principle as the no-op
scenario check — a comparison with nothing in it is not a passing comparison.

The stricter comparison is also more sensitive: the self-test in section 2 now
catches four scenarios where it caught three.

### Recovery that could never happen

The real run showed `recoveries: 0` in every damaged scenario, including ones
where the book had visibly converged. The criterion was N *consecutive*
resolvable references, resetting to zero on any miss.

On the synthetic feed that works, because orders churn fast and the pre-gap
tail dies out in seconds. On a real MSFT day it never fires: an order added
before the gap can rest for **hours**, and one cancel of one such order at
15:45 wipes out a run that had climbed to nineteen thousand. The book had long
since converged; the criterion could not say so.

It is now a rate over a sliding window — at most 5 unresolved in the last
50,000 references, one straggler per ten thousand. Two tests pin both halves:
a lone straggler every few hundred messages must not block recovery forever,
and a sustained 2% unresolved rate must still block it.

---

## 4. What recovery actually means here

The plan says "request/replay a snapshot". That is the one clause this project
cannot honour literally, and pretending otherwise would be the worst thing in
the phase.

NASDAQ's real answer is two services we do not have: a retransmission server
that resends sequence numbers on request, and GLIMPSE, a snapshot feed carrying
the current book. Working from a historical file there is no one to ask. A
snapshot we took ourselves is no help either — it predates the gap, so it is
stale by exactly the messages we are missing.

So recovery is **rebuild-forward**, and its property is precise:

> The rebuilt book contains **no wrong orders**, only missing ones. Every order
> in it was added by a message we actually received. It is a subset of the
> truth — a quote from it is never invented, only sometimes absent.

And it converges, with the feed itself supplying the signal. Every message
deleting, executing or replacing an order the book has never heard of is a
message about the pre-gap world we discarded. As those orders age out of the
real market that rate falls to zero, and when it has been zero long enough,
everything still resting is something we watched arrive. `unknown_ref` is not a
diagnostic counter here; it is the recovery signal.

The table in section 3 is that convergence measured: up to 16,231 messages lost
in a single hole, the book comes back byte-identical.

---

## 5. Restart

Snapshots are useless for a gap and exactly right for the other discontinuity
in the plan — "reconstruct position and open orders after a mid-day process
restart". The difference is where the missing information is. After a gap the
bytes were never received. After a crash they were received and processed, and
what was lost is memory, which is a thing you can write down.

The property, asserted at **every** cut point in the stream rather than one
convenient one:

> snapshot at message N, restart, replay from N
> == a process that ran continuously through N

Identical: same orders, same queue positions, same tape. The detail that makes
or breaks it is order. Orders are written in fill order — best price first,
oldest first within a price — and restored by replaying adds, because `add()`
appends. Dump the ref map in hash order instead and you restore a book with the
right levels, the right shares at every level, and scrambled queue priority:
every level-based check passes, every phase 6 queue model is silently wrong.
That case has its own test.

A truncated or wrong-version snapshot is refused rather than half-loaded. A
partial restore produces a book that looks entirely plausible.

---

## 6. The invariant that is only true some of the time

`bid < ask` is the first thing anyone asserts about an order book, and it is
false during every halt. The book keeps accepting orders — that is what the
quotation-only period is *for*, it is how the reopening price is discovered —
and nothing executes to clear them, so it crosses. It uncrosses at the resume,
when the halt cross runs as one auction and prints as a `Q` with type `H`.

So the check is conditional, and both halves matter:

* crossed while **halted** — expected; counted, reported, not an error
* crossed while **trading** — a defect, and the thing the unconditional
  assertion was reaching for

A single check that ignores session state either misses the second case or
drowns in the first. "Never silently wrong" is the requirement, and an alert
nobody reads is a quieter kind of silence than no alert at all.

One more case, and it is specific to this phase: a process that starts up while
a symbol is already halted goes `Unknown -> Halted`, which is not a halt it
witnessed. Counting it would have a restarted process inventing an event that
happened before it existed. Both transitions are guarded on having actually
seen the previous state.

---

## 7. The kill switch

Five limits, each independently configurable, all latching — once tripped they
stay tripped until explicitly reset, because a risk control that silently
re-arms is how you get the same incident twice.

The distinction the design turns on: **pre-trade suppression and a post-trade
trip are different controls and you need both.** Phase 6 had only the first. A
perfect pre-trade check still lets you breach, because fills arrive for orders
that are already resting and no check at decision time can prevent that.

---

## 8. What this establishes, and what it does not

Established:

* Ten damage scenarios, and in none of them does the system report a trusted
  book that differs from the truth.
* The grader detects silent wrongness when it is present — demonstrated by
  inducing it, not assumed.
* Rebuild-forward converges from a 16,231-message hole to a byte-identical
  book, and the convergence signal comes from the feed.
* Restart equals continuous operation at every cut point, queue priority
  included.
* A crossed book is classified by session state, so halts do not generate
  false alarms and a genuinely crossed trading book is not lost among them.

Not established:

* **That any of this holds on a real day.** Every number here is from a
  synthetic feed. Phase 6 is the cautionary tale: its headline multiple was a
  property of the generator and did not survive MSFT. Section 9 is the command.
* That the reorder buffer's depth and patience suit any particular network.
  They are matched to a path, and this project has no path.
* That rebuild-forward is the right policy for a live system. With a
  retransmission service available, asking for the missing sequence numbers is
  strictly better, and the honest reason it is not implemented is that there is
  nobody to ask.
* Anything about latency under recovery. The book rebuild is not on a measured
  path.

---

## 9. On a real day

```
./scripts/real-data-run.sh 12302019.NASDAQ_ITCH50.gz MSFT 50
```

The script runs the adversarial matrix against the real feed after the phase 6
measurements. On a real trading day the outage resolves to an actual 14:00 —
the plan's scenario as literally specified, for the first time — with a full
book behind it and two hours of session still to come, which makes it a harder
test than any synthetic feed can pose.
