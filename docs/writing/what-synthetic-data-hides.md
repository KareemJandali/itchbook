# What synthetic data hides

*Notes from building an order-book reconstructor and a queue-position
backtester against NASDAQ TotalView-ITCH.*

Every result in this project existed twice: once on a feed I generated, and
once on a real trading day. The two agreed less often than I had expected, and
the disagreements taught me more than the agreements did. What follows is a
list of the occasions on which a number was true on synthetic data and false on
a real day. For each one I give the mechanism I eventually found, and I close
with what I would do differently.

The setup, briefly. A C++20 reconstructor reads raw ITCH and maintains a
per-order book. A deliberately slow Python implementation does the same thing
independently, and the two are required to produce byte-identical output. On
top sits a backtester that answers a question the feed cannot: *if my order had
been resting there, would it have filled?* Nothing in a public feed resolves
whether a cancel was ahead of you or behind you, so the backtester runs four
fill models at once and reports the range.

Real data here means MSFT on 30 December 2019, which is 1,221,484 messages for
that one symbol.

---

## 1. The headline number was a fact about my generator

For a while the project's marketing claim was this. A naive backtester, meaning
one that fills you whenever anything trades at your price, reports **3.67×** the
P&L of a pessimistic one that makes you wait your turn. That is a satisfying
number. It says queue position is worth a factor of nearly four, and it came
from a clean experiment: same strategy, same bytes, four fill models, one pass.

On MSFT the same measurement gives **0.87×**.

Worse, the ratio is the wrong statistic entirely. On real data the strategy
*loses money*, so both numbers are negative, and a ratio of two negative numbers
says nothing about which one is flattering. My tooling printed `0.87x` under a
caption reading "the cost of assuming you are at the front of every queue",
which was simply false. The honest statistic is the signed difference: naive
reports **$401 more** than pessimistic, so it still flatters, by a margin
nothing like a factor of four.

What survived was the *direction*. Naive over-fills and never under-fills; that
is structural, and it held. The magnitude did not survive, and the magnitude was
the part I had been quoting.

**The mechanism.** My generator's price was a driftless random walk whose levels
refilled after every sweep. A maker filled during a sweep gets marked at the
bottom of it and then watches the price come back. A real book does not do that.

---

## 2. Three presentation bugs, all on the loss side

When the strategy started losing money, three separate pieces of my analysis
code began lying, and every one of them lied in the same direction.

The ratio caption above was the first. The second was in the latency sweep. It
reported "surviving fraction of the zero-latency number", computed as
`last / first`. With a profit that reads correctly. With a loss that grew by
77%, it printed **1.77** under a heading where 1.0 meant "flat", which is the
best possible reading of the worst possible outcome.

The third was a chart. Bars for negative values grow leftward from zero,
straight into the category labels, so the longest bar rendered as
`mbo3,425.4`, with the minus sign hidden behind the word "mbo". The largest loss
in the figure read as a gain.

None of these were reachable on synthetic data, because there the strategy was
profitable and every bar pointed the other way. **Every presentation bug I found
was on the loss side**, which is the side that matters: a backtester that makes
losses look smaller is failing in the one direction you cannot afford.

---

## 3. A receiver that reported a clean session having lost 40% of the day

The recovery work injects packet damage into a MoldUDP64 stream, including
drops, duplicates, reordering, truncation and a mid-day outage, and it grades
each run. Did the book match an undamaged replay, and did the system *say* it
was trustworthy? The only unacceptable outcome is a book that differs while
claiming to be trusted.

Ten scenarios at the time, no failures. Then, instead of asking "does it
survive this list", I asked "how much can it survive", and swept the outage
length until something broke. At 2,000 packets it did:

```
0 messages lost, 0 gaps, state: trusted
```

with **80,235 messages missing, 40% of the session.**

**The mechanism.** A gap in the middle of a stream announces itself, because the
next packet carries a sequence number that has jumped. A stream that *dies*
never sends that next packet. There is no discontinuity to notice, and every
counter stays honestly at zero. Nothing was detected because from sequence
numbers alone nothing was detectable.

MoldUDP64 already carries the answer in an end-of-session marker, so a stream
that ends without one did not end, it stopped. That is now treated as a halt and
not as a recovery, because there is no bound on what was missed and no further
messages with which to demonstrate convergence.

The scenario matrix could not have found this. Every outage in it was short
enough that the feed came back. **A scenario list checks the cases you thought
of; a sweep finds the edge.**

---

## 4. A recovery criterion that could never fire on a real book

With no retransmission service to ask, recovery from a gap is *rebuild-forward*:
discard the book and rebuild from the next message. The resulting book contains
no wrong orders, only missing ones, so it is a subset of the truth and never an
invention.

It also converges, and the feed itself supplies the signal. Every message
deleting or executing an order the book has never heard of is a message about
the pre-gap world we discarded. As those orders expire, that rate falls to
zero, and when it has been zero long enough the book is whole again.

I implemented "long enough" as *N consecutive resolvable references, reset to
zero on any miss*. On the synthetic feed that works, because orders churn fast
and the pre-gap tail expires within seconds. On MSFT it never fires at all. An
order added before the gap can rest for **hours**, and one cancel of one such
order at 15:45 destroys a run that had climbed to nineteen thousand. The book
had long since converged; the criterion could not say so.

It is now a rate over a sliding window, at most 5 unresolved in the last
50,000 references. **A consecutive run is a fragile way to measure a rate**, and
only data with long-lived orders in it shows that.

---

## 5. And then real data broke the fixed version too

Section 4's story has a second half that I would rather report than omit. With
the windowed criterion in place, the synthetic feed converges from a
16,231-message hole back to a byte-identical book. I wrote that down as the
recovery property.

On MSFT, **it never converges at all.** Every damaged scenario finishes the
session still recovering.

This one is not a bug, and the distinction matters. The verdicts are correct:
the book differs, and the system says so. It is a fact about real books. A
rebuild at 14:00 discards several thousand orders resting at that moment, and on
a real name those keep being cancelled and executed for hours, so the pre-gap
tail outlives the session. The synthetic feed converged because it is two
minutes long and churns its whole book many times over.

So the property is narrower than I first stated it: *no wrong orders, only
missing ones* holds unconditionally; whether it re-converges within the session
is a question about the symbol and the hour.

---

## 6. The comparison harness invented a disagreement

The reconstructor's headline claim is byte-identical output against the Python
oracle across a full trading day. Pointed at MSFT it reported 61,228 snapshot
rows identical and then failed on two summary fields, `best_bid` and `best_ask`,
where one implementation said `None` and the other said `-1`.

Both were right. MSFT's book is **empty at the close**, every order cancelled,
so there is no best bid. One implementation spelled that as null and the other
as a sentinel. The bug was in the comparison harness, and not in either book.

A search for more of the same turned up four, and one worse. OHLC and VWAP are
undefined until something trades and got the same sentinel treatment, and
`trading_state` is `'\0'` until the feed says otherwise, which I printed raw,
putting a NUL byte inside a JSON string. That file did not merely disagree, it
did not parse.

None of it was reachable from a generated feed. **They all end with orders still
resting and something having traded**, so every field has a real value and every
spelling matches. It took a day that ends the way real days end.

---

## 7. Two quadratics that only a real day made visible

Not every finding was about correctness. A six-point latency sweep over MSFT
took close to an hour. Every test passed throughout, because they are all about
correctness and a quadratic is not wrong, only ruinous.

Two structures were being walked per message and growing with the run. The first
held retired orders that were marked dead but never erased from the list they
lived in, and the second held markout samples awaiting a horizon. The second
accounted for 40 of the 47 seconds a single backtest took. One frontier index
advanced only when a sample had *all* horizons resolved, so with a ten-second
horizon it rescanned every fill of the last ten seconds, on every message.

The sweep went from ~50 minutes to **4 seconds**, results byte-identical either
way.

Two things about how this was found. First, I guessed wrong twice, since two
plausible hypotheses both measured flat, and what settled it was a sample of the
stack under `gdb` and one rebuild with `-g` to name the line. Second, the project
had a benchmark for the *book* and none for the *simulator*, so nothing measured
the cost of the thing that got slow. There is now a CI check that compares two
feed sizes and asserts the ratio: double the input, linear costs ~2×, quadratic
~4×, and the machine's speed cancels out. The pre-fix code measures 3.83×
against a limit of 3.0, so the guard is known to catch what it exists for and is
not merely assumed to.

---

## 8. The fuzzer reported full coverage of two thirds of an engine

Everything above is a synthetic *feed* misleading me about a real day. This one
is worse, because the generator was not modelling reality at all. It was
producing random input, which is supposed to be the honest kind.

The matching engine implements six order types: limit, market, IOC, FOK,
stop-market and stop-limit. A property fuzzer ran a million random sequences on
every push and checked four invariants after every operation. The book is never
crossed, shares are conserved, states move only where the machine permits, and
price-time priority holds. It had run tens of millions of sequences without a
violation, and I quoted that in the README as evidence the engine was sound.

The generator picked a type with `switch (in.u8() % 8)`, and the arms of that
switch covered four types. It had never emitted a stop, of either kind, ever.
`StopLimit` also had no unit test anywhere, so the strongest evidence in the
project covered four of six types while reporting on all six.

When stops were added to the generator it broke on the first run, twice. A stop
that fires into an empty book attempted a transition the state machine forbids,
`Accepted -> Rejected`, where `Rejected` means "never touched the book" and is
reachable only from `New`, so it did not return an error, it tripped an assert
and killed the process. And a triggered stop kept the arrival sequence it had
been given when it was *parked*, so a stop dormant since the open could rest
ahead of orders that had been queued at that price all morning.

I fixed the second by giving a triggered stop a fresh sequence at trigger time.
That is correct. It is also what made the next bug invisible.

Underneath sat a third defect. The routine that fires elected stops removed them
from its pending list by swapping the last element into the vacated slot. Park
three stops, elect all three with one trade, and they rest in the order 1, 3, 2.
The sequence fix meant the fuzzer's price-time-priority invariant no longer saw
it, because the numbers it compared were now assigned at election, so they
agreed with the wrong order. Revert that one line and the fuzzer fails within
fifty iterations. For as long as it took to look again, the repository sat in a
state where a real bug had gone from *detectable* to *silent*, and the commit
that did it was a bug fix with a green test suite.

That is the failure mode this entire project is organised against, produced by
the machinery built to prevent it. The repair is unglamorous: remove elements in
place, then add two tests that drive the queue directly and check who actually
fills first. The lesson concerns the fuzzer and not the vector. **A fuzzer that
stops covering something does not report a gap. It reports a pass.** The
generator now counts what it emitted and fails the run if any type's count is
zero, which turns coverage from something I believed into something the build
checks.

---

## What I would do differently

**Generate the failure, not the shape.** My feeds reproduced a real day's
*message mix* and its *queue structure*. What they did not reproduce was how a
day *ends* (empty book, nothing trading), how long a real order *rests* (hours,
not seconds), and how a real price *moves* (not back to where it started). Every
finding above traces to one of those three.

**Sweep the parameter, do not enumerate the cases.** The scenario matrix found
nothing the code did not already handle. A sweep of outage length, continued
until it broke, found the one failure that mattered. A list tests your
imagination; a sweep tests the system.

**Make the harness fail on purpose.** After the grader had returned only
CORRECT and SAFE, I had no evidence it *could* return WRONG. A convergence bar
set to a single reference produces five WRONG verdicts and a non-zero exit, and
that run is in CI alongside the real one. A check that has never failed is not
yet a check; it is an assertion about a check.

**Make coverage a failing condition, not a footnote.** A property fuzzer's
output is "no invariant violated", and that sentence is equally true of an
engine that is correct and of one that was never exercised. Every generator I
write now reports what it produced and fails when a category comes back empty.
"What did this run *not* test?" has to have an answer the build can check,
because nobody reads a passing log.

**Watch for fixes that silence the detector.** The most dangerous commit in this
project was a correct bug fix. It repaired a real defect and, as a side effect,
stopped a different one from being visible, and it shipped green. When a fix
makes a check stop firing, the question is not "does it pass now" but "would
this check still catch the thing it was written for". A one-line revert to
confirm the harness still fails costs a minute, and it is the only way to know.

**Distrust the flattering direction specifically.** Three separate bugs
appeared the moment a strategy started losing money, and all three made the
loss look smaller. That is not a coincidence: the profitable case is the one you
develop against, so the loss path is the one that never gets exercised. Run the
strategy you *know* is bad, early, and read the output as adversarially as you
would read someone else's.

---

*Source, with every number above reproducible from a clean clone:*
[github.com/KareemJandali/itchbook](https://github.com/KareemJandali/itchbook)
