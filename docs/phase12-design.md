# Phase 12 — design decisions, made before the gateway exists

*Written before any of `include/itchbook/ouch/`, `engine/gateway.hpp`, or the
emitting matcher. The plan requires this order and gives its reason: the
topology determines what the gateway is **for**, and a gateway written first
would encode a topology nobody chose.*

Nothing here is a measurement. Each item is a decision, together with its cost
and the test that would prove it wrong.

---

## 1. What this document decides

Phase 12 puts a strategy on one side of a socket and an exchange on the other,
inside a replayed historical trading day. Four questions have to be settled
before a line of protocol code is written, and the build plan answers only the
first:

1. **How does historical flow enter the system**: as orders to be matched, or as
   state to be applied? (§2, §3)
2. **Who owns the book**, given that historical orders and strategy orders rest
   in the same price levels and compete in the same queues? (§4)
3. **What stops a strategy order reference from colliding with a historical
   one**, when both are published on the same feed to the same consumer? (§5)
4. **Which clock is which**, when the market runs in replay time and the latency
   being measured is wall clock? (§6)

Question 1 is where v1.0 was wrong. Questions 2–4 are consequences that appear
only once question 1 is answered correctly, and an error in any of them produces
a system that runs and publishes plausible bytes while measuring nothing.

---

## 2. The rejected topology, and the real reason

v1.0 proposed replaying the recorded day's orders into the matcher as
exchange-side flow: every historical `A` becomes an order submitted to your
matching engine, and your engine reproduces the day.

**It does not reproduce the day, and it fails even with zero strategy orders.**

A historical add was non-marketable *against the real book at the instant it was
entered*. Your book is a different book. ITCH publishes executions as **facts**,
and a matcher fed only adds has no crossing events to consume, so its book
drifts away from the real one from the first execution onward. Adds that were
resting orders in reality arrive at a diverged book and **cross**. The emitted
stream then diverges for reasons that have nothing to do with any strategy, and
the backtester-modelling-error table of §12.4, the deliverable that makes phase
12 an experiment rather than a demo, is dominated by that artifact.

The failure is that the error term you are trying to measure gets swamped by an
error you introduced yourself, and the two are not separable after the fact. The
problem is deeper than an imperfect simulation.

---

## 3. The decision: adds are state, executions are events

The build plan's fix is that historical flow "replays as book state, through the
phase-9 path you already trust." That is correct, and it is also not sufficient,
because taken literally it makes the strategy's orders **unfillable**.

Consider a historical execution: `E` says order reference *R* traded *N* shares.
When that is applied as state, order *R* loses *N* shares and your resting order
is untouched. Do the same for every execution in the day and your maker never
trades, because the aggressor that would have taken your order appears in the
feed only as its consequence on someone else.

So the message classes are not treated alike:

| ITCH message | treatment | why |
|---|---|---|
| `A`, `F` add | **applied as state** | non-marketable when entered; matching it against a diverged book is what v1.0 got wrong |
| `U` replace, `X` cancel, `D` delete | **applied as state** | they name an existing order and remove or move liquidity; no crossing occurred |
| `E`, `C` execution | **replayed as an aggressor through the matcher** | an execution *was* a crossing event — replaying it as one is faithful |
| `P` non-cross trade | **replayed as an aggressor**, non-displayed | same argument; it consumed displayed liquidity that is not otherwise accounted for |
| `Q` cross | **applied as state** | an auction clears at a single price by its own rules; it is not a book walk |
| `R`, `H`, `S`, `h`, `W`, `B` | **applied as state** | already the phase-9 path |

**This is the whole design in one line: an add is a fact about the book, an
execution is a fact about a trade, and only the second is a crossing event you
are entitled to replay as one.**

The aggressor is synthesised with the historical execution's **size and price**,
and it walks the book from the front of the queue. If your order is ahead of *R*,
the aggressor takes your shares first. That is exactly the fill the queue model
in phases 6 and 11 was *estimating*, and here the matcher executes it against a
real book.

One consequence must be stated, because it is a divergence and not a bug. When
the aggressor consumes your shares, the historical order *R* ends the day
holding more shares than history records. The historical `E` is **consumed by
the aggressor path and never also applied as state**, since applying both would
double-count the trade. The size of that divergence is bounded by the shares
your orders absorbed, and it is attributable order by order, which is what makes
the table in §12.4 meaningful rather than mysterious.

---

## 4. One book, two sources of mutation

There is exactly **one order book per symbol** on the exchange side. It holds
historical orders and strategy orders in the same price levels, in arrival order.

The choice is structural, and not a convenience. Queue position is the entire
subject of phases 6 and 11, and it exists only if your order and the historical
orders it competes with sit in one queue. Two books, a "market" book the matcher
consults and a "mine" book it owns, would put your order in a queue of one and
give it a fill rate nothing in reality supports.

The two sources of mutation are kept distinguishable at the order level, and not
by holding separate books:

- **The replayer** applies historical adds, replaces and cancels directly, and
  submits synthesised aggressors for historical executions.
- **The gateway** submits strategy orders through the matcher's ordinary path,
  with the risk layer in front of it.

Both paths mutate the same book through the same code. The reason goes beyond
tidiness: the emitted ITCH is generated from book mutations, so a mutation that
avoided the matcher would be a mutation the published feed never described, and
the strategy's own reconstruction would then diverge from the exchange's with no
signal that it had.

---

## 5. Reference space is partitioned, and the partition is checked

Historical order references come from NASDAQ and are arbitrary 64-bit values. The
matcher assigns references to strategy orders. Both are published on the same
feed to the same consumer, and a collision does not fail loudly: it corrupts the
strategy's book at a single price level, and the run continues.

This is phase 12's version of the locate trap, and it gets the same treatment:

- **Strategy references occupy the high half of the space** (bit 63 set).
  Historical NASDAQ references have never been observed in that range, and
  "never observed" is a claim about a sample, so:
- **the replayer asserts** that no historical reference has bit 63 set, on every
  message, and aborts the run if one does. A run that cannot guarantee the
  partition must not produce numbers.
- The emitted `A`/`F` for a strategy order carries the same `stock_locate` as the
  symbol's historical messages, because it is the same symbol on the same venue.
  There is no second locate space.

The token↔reference map in the gateway (OUCH tokens are client-side; ITCH
references are exchange-side) is a separate mapping, and it is no substitute for
the partition. A token collision is a protocol error the gateway rejects, while
a reference collision is a silent book corruption downstream.

---

## 6. Two clocks, deliberately

The system runs on two independent time bases, and any conflation of them
produces numbers that look reasonable and mean nothing.

**Replay time** is the market's clock. The historical feed's own nanosecond
timestamps drive it, the replayer advances it, and every emitted ITCH message is
stamped with it, including the messages the matcher generates for strategy
orders. A day replayed at 10× real time still produces a stream whose internal
timestamps describe the original session, because a consumer's book arithmetic
depends on that.

**Wall clock** is the measurement clock. Tick-to-trade, the headline of §12.3, is
the real elapsed time your code takes, from packet arrival at the strategy socket
to OUCH enter on the wire. It has nothing to do with replay time and must never
be derived from message timestamps.

Two rules follow:

1. **Replay speed is a parameter of the experiment and is reported with every
   latency number.** A tick-to-trade measured while replaying at 50× is measured
   under a message rate the original day never had; that is a legitimate stress
   test and an illegitimate thing to report unlabelled.
2. **The strategy's decisions are functions of replay time; its latency is
   measured in wall clock.** An A-S maker's `(T − t)` horizon is session time,
   not machine time. If it were fed wall clock during a 50× replay, its horizon
   would collapse by a factor of fifty, which changes the strategy rather than
   the load.

---

## 7. What the gateway is therefore for

With §3–§6 settled, the gateway's job is narrow and can be stated:

1. **Terminate a SoupBinTCP session**: login, sequenced data, heartbeats both
   ways, end of session. Heartbeat death is wired to the kill switch, so a dead
   session flattens, and that is a test with a cancel count in it rather than a
   hope.
2. **Validate OUCH and answer every inbound message exactly once**, with `A`,
   `U`, `C`, `E` or `J` and a reason code. An unknown token, a bad price or a
   wrong state produces a rejection, not an exception.
3. **Own the token↔reference map**, and assign references inside the strategy
   partition of §5.
4. **Apply the risk layer before the matcher, not after**: `risk/kill_switch.hpp`
   in line, a price collar against the current book, a maximum order rate and a
   maximum position. Risk that runs after matching is accounting, not risk.

One exclusion is explicit: the gateway does not see historical flow. The replayer
does not speak OUCH and never enters a session. That asymmetry makes the §3
decision structural, since historical participants are state and events rather
than counterparties with sessions.

---

## 8. What this costs, stated here rather than in a limitations section

- **Historical participants never react to you.** They quoted and pulled against
  a market that did not contain your orders. A maker that improves the touch for
  an hour would, in reality, have been joined or faded within milliseconds. This
  is not modelled and cannot be modelled from a recorded feed.
- **Your fills consume liquidity nobody in the recording lost.** Per §3, when
  your order is hit the historical order behind you keeps shares it did not keep.
  Bounded and attributable, but real.
- **No market impact model, in either lane.** The omission is deliberate. An
  unvalidated impact model would add an error term with no ground truth to check
  it against, which is worse than a stated absence.
- **The claim "real flow trades against my venue" is given up.** What remains is
  an experiment whose disagreements are attributable, and §12.4 exists to use
  that trade.

---

## 9. Predictions, written before anything is built

Standing rule 2. These are graded in `docs/phase12-results.md`, kept or falsified
in print.

- **P1, the zero-strategy identity.** With no strategy orders entered, the book
  built by consuming the *emitted* feed is byte-identical to the book phase 9
  builds from the *original* feed, for a full session. This is the sharpest
  available test of §3. If adds-as-state plus executions-as-aggressors is the
  right decomposition, then replaying a day through it and re-deriving the book
  is a round trip that must close exactly. **If P1 fails, §3 is wrong and the
  topology is reconsidered from the start rather than given a tolerance.**
- **P2, bounded divergence.** With strategy orders, the difference between the
  emitted book and the phase-9 book is accounted for, share for share, by
  strategy fills and the historical orders they displaced. No unexplained
  residual remains.
- **P3, the hops sum.** The per-hop histograms of §12.3 sum to the end-to-end
  tick-to-trade at p50, within the measurement resolution. A gap means a hop
  nobody is timing.
- **P4, the backtester is optimistic.** Live fills through the loop will be
  *fewer* than the phase-11 `mbo` lane predicts for the same day and strategy,
  because the queue model resolves ties in the strategy's favour more often than
  a real matcher does. The direction is predicted and the magnitude is not.
- **P5, determinism holds under load.** A fixed inbound script produces
  byte-identical emitted ITCH across runs and across replay speeds. If replay
  speed changes the output, something is reading wall clock where it should read
  replay time (§6).

P1 and P5 are CI gates from the first commit of the matcher, not end-of-phase
checks. They are cheap, they fail loudly, and every later number depends on them.

---

## 10. Deliberately not decided yet

Recorded so that a later choice is visible as a choice rather than a default:

- **OUCH version.** 4.2 is simpler and is the plan's leaning, although the field
  table is built from the spec before the version is fixed in writing.
- **Whether the aggressor carries the historical execution's price or the
  book's.** The two differ for `C` (executed with price), which is precisely the
  message that says the trade printed away from the resting price. This needs
  the message-level detail work of §12.1 before it can be settled honestly.
- **Hidden liquidity.** The feed shows non-displayed executions but never the
  resting hidden order. Whether the exchange models hidden interest at all, or
  treats `P` purely as an aggressor that consumes displayed shares, is open.
- **Multi-symbol.** Everything above is per symbol. Whether one exchange process
  serves many symbols or one process per symbol is a scaling question with no
  bearing on correctness, and it is not answered here.

---

## 11. What would make this document wrong

The decision in §3 rests on one empirical claim: that a historical day, decomposed
into applied state and replayed aggressors, reconstructs to the same book as the
original feed. That claim is P1. It is testable before any strategy exists, and
it is testable on data already in hand.

**Build P1 first**, before the OUCH field tables, before SoupBinTCP and before
the gateway. It needs the replayer, the matcher's existing `submit`, and the
phase-3 book, all of which exist. It decides whether the rest of phase 12 is
built on a topology that works or on one that merely sounds better than v1.0's.
