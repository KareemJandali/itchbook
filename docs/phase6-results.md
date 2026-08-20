# Phase 6 — Queue-position backtester: results

Same strategy, same bytes, four fill models, one pass. The gap between them is
what the phase exists to measure, and everything below is reproducible from the
commands in each section.

Everything here runs on a **synthetic feed**. That is a real limitation and it
is stated again at the end rather than buried: the numbers demonstrate that the
machinery measures what it claims to, not what MSFT did on a Tuesday. Section 7
says exactly which conclusions survive the substitution of real data and which
do not.

```
python3 python/make_queue_feed.py data/raw/queue_long.gz --messages 200000 \
    --gap-ns 300000 --seed 7
```

---

## 1. The headline

```
./build/queue_backtest data/raw/queue_long.gz --strategy touch-maker \
    --max-position 1000 --json docs/figures/touch-maker.json
python3 python/analysis/fill_comparison.py docs/figures/touch-maker.json \
    --svg docs/figures/fills-total.svg
```

200,056 events. A symmetric maker at the touch, 100 shares a side, held to a
1,000-share position limit, at the default 250 µs one-way latency.

| model | fills | shares | P&L | c/share | edge c/sh | fees c/sh |
|---|---:|---:|---:|---:|---:|---:|
| naive | 8,624 | 521,345 | $8,423.15 | 1.6157 | 1.3515 | −0.0903 |
| optimistic | 3,065 | 207,061 | $2,403.18 | 1.1606 | 1.0162 | −0.0900 |
| mbo | 2,997 | 204,832 | $2,366.28 | 1.1552 | 1.0079 | −0.0900 |
| pessimistic | 2,960 | 199,791 | $2,293.93 | 1.1482 | 1.0021 | −0.0900 |

**Naive claims 3.67× the P&L of pessimistic.** Almost all of that is volume: it
reports 2.6× the shares. The rest is worse — naive also claims a third more
edge *per share*, because the fills it invents happen at moments the queue never
actually reached, and those are systematically the good moments.

`mbo`, which resolves order references instead of guessing, sits inside the
optimistic/pessimistic band. That is the check that says the band is a band and
not two arbitrary numbers with a gap between them.

![Total P&L by fill model](figures/fills-total.svg)
![Shares filled by fill model](figures/fills-shares.svg)

The two panels are both here on purpose. Per share the four models agree to
within a tenth of a cent, so a per-share chart alone shows four identical bars
and hides the entire result.

---

## 2. The external check: shadow real orders

Everything in section 1 is internal. Two implementations agreeing and
invariants holding would both pass if the implementation were wrong in a
consistent way. This is the one check with an answer that does not come from us.

```
python3 python/analysis/leave_one_out.py data/raw/queue_long.gz \
    --binary build/queue_sim --samples 200 --partial-only
```

For a real order O, replay the feed **unchanged** with a simulated order of O's
price and size placed one message before O's add — so it stands where O stood,
with the same queue ahead of it — and pulled one message before whatever removed
O. The feed says how many of O's shares filled. Compare.

Nothing is edited, so no other participant's behaviour changes and the queue
ahead is the real queue. O now sits directly behind us, so every execution the
tape addresses to O is flow that reached the front of that queue while we were
at the front of it.

200 orders that were pulled **part-filled** — the discriminating cases; an order
that ended fully executed grades every model perfect and measures nothing:

| model | mean error | mean abs error | over | under | exact |
|---|---:|---:|---:|---:|---:|
| naive | +66.3 | 66.3 | 139 | 0 | 61 |
| optimistic | +27.4 | 27.4 | 61 | 0 | 139 |
| **mbo** | **0.0** | **0.0** | **0** | **0** | **200** |
| pessimistic | −57.7 | 57.7 | 0 | 98 | 102 |

**Bracketed by [pessimistic, optimistic]: 200/200.** The band is a bound.

`mbo` is exact on every one of the 200. `naive` over-fills by 45% of the average
order and never under-fills, which is the shape the theory predicts: a model
that ignores the queue can only ever be at least as filled as one that waits.

This oracle found two defects in the feed generator before it found anything
about the models, and both were real:

* Executions were happening at randomly chosen price levels. A print below a
  resting bid is a tape no single venue produces, and the simulator was then
  *correct* to fire price priority on nearly every fill — a shadow order took
  100 shares where the order it shadowed took 23.
* Executions were happening through halts. A halted symbol does not trade, so
  the models correctly ignored those messages, their idea of what was ahead
  stopped matching the book, and they reported zero for the rest of the
  order's life.

---

## 3. The closed form: a feed where every fill is adverse

```
python3 python/make_toxic_feed.py data/raw/toxic.gz --episodes 40
./build/queue_backtest data/raw/toxic.gz --strategy touch-maker --json toxic.json
python3 python/make_toxic_feed.py --check toxic.json --episodes 40
```

A feed engineered so that every quantity is fixed before the simulator runs.

| model | fills | shares | edge c/sh | drift 100ms | drift 1s | drift 10s | unresolved | residual |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| naive | 40 | 4,000 | 1.0000 | −2.0000 | −2.0000 | −2.0000 | 0 | 0 |
| optimistic | 40 | 4,000 | 1.0000 | −2.0000 | −2.0000 | −2.0000 | 0 | 0 |
| mbo | 40 | 4,000 | 1.0000 | −2.0000 | −2.0000 | −2.0000 | 0 | 0 |
| pessimistic | 40 | 4,000 | 1.0000 | −2.0000 | −2.0000 | −2.0000 | 0 | 0 |

Gross P&L is **exactly zero** in every lane. Every fill is worth minus one cent
measured against the mid ten seconds later, and yet the realized round trip
breaks even: we buy the bid at mid − 1 tick, the mid drops two ticks, and the
ask we then sell is the new mid + 1 tick — the same price. A two-tick spread
against a two-tick adverse move nets zero. That is the textbook statement of
what adverse selection does to market making, and the rebate is all that is
left. A markout and a P&L answer different questions.

All four models agree here, because the feed contains no queue ambiguity. A
backtester whose models disagreed on this feed would be finding ambiguity that
came from its own implementation.

---

## 4. Latency

```
./build/latency_sweep data/raw/queue_long.gz --strategy touch-maker \
    --max-position 1000 --csv docs/figures/latency-touch-maker.csv
python3 python/analysis/latency_sweep.py docs/figures/latency-touch-maker.csv \
    --svg docs/figures/latency-pnl.svg --shares-svg docs/figures/latency-shares.svg
```

Shares filled, by one-way latency:

| model | 0 µs | 100 µs | 250 µs | 500 µs | 1 ms | 2 ms | 5 ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| naive | 569,207 | 538,224 | 521,345 | 499,960 | 475,831 | 447,342 | 413,789 |
| optimistic | 181,857 | 196,596 | 207,061 | 216,973 | 223,577 | 222,206 | 219,574 |
| mbo | 179,358 | 193,673 | 204,832 | 214,678 | 221,663 | 220,272 | 218,270 |
| pessimistic | 173,494 | 189,145 | 199,791 | 210,049 | 217,765 | 217,411 | 215,995 |

P&L, by one-way latency:

| model | 0 µs | 100 µs | 250 µs | 500 µs | 1 ms | 2 ms | 5 ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| naive | $9,239 | $8,707 | $8,423 | $8,074 | $7,717 | $7,296 | $6,822 |
| optimistic | $2,080 | $2,256 | $2,403 | $2,520 | $2,646 | $2,686 | $2,660 |
| mbo | $2,062 | $2,222 | $2,366 | $2,495 | $2,613 | $2,666 | $2,645 |
| pessimistic | $1,939 | $2,141 | $2,294 | $2,425 | $2,565 | $2,606 | $2,586 |

The two directions are the interesting part.

**Naive decays with latency and the queue models improve.** Naive loses 27% of
its fills between 0 and 5 ms, because its fills come from being present when a
trade prints at its price, and arriving later means being present less often.
The queue models gain 24%, because this strategy requotes whenever the touch
moves and a replace goes to the back of the queue — latency slows the churn, so
each order rests longer and climbs further. Under a model where queue position
is real, being slower to requote is worth something.

That is not an argument for slow infrastructure. It is an argument that
TouchMaker requotes too eagerly, and it is only visible at all because the
queue models charge for a replace.

![P&L vs latency](figures/latency-pnl.svg)
![Fill volume vs latency](figures/latency-shares.svg)

Neither curve collapses toward zero, so nothing here is a latency edge.

`patient-maker`, which joins the touch and holds until the price walks five
ticks away, makes that reading sharper. Its fills are *identical* at 0 µs,
250 µs and 1 ms — 4,300 / 4,200 / 4,200 shares for optimistic / mbo /
pessimistic at every one of those points — and only move at 100 ms and 1 s. An
order that rests for seconds does not care about microseconds, and saying so
with a flat line is more useful than a single number at one latency. The price
of that flatness is volume: it fills 4,300 shares where touch-maker fills
207,000, because it is holding one quote a side rather than chasing the touch.

---

## 5. The controls

A backtester that cannot produce a loss is broken.

**Crossing the spread every N events** (`--strategy crosser`), 33,831 shares:

| P&L | c/share | edge c/sh | fees c/sh |
|---:|---:|---:|---:|
| −$714.11 | −2.1108 | −1.5407 | +0.4099 |

Identical in all four models, to the cent. Crossing the spread is not a queue
question, so a difference between lanes here would mean fill-model state was
leaking into the taker path.

**Quoting 200 ticks from the touch** (`--strategy far-quoter`): zero fills, zero
P&L, in every model. Any fill there would mean the simulator reached a price the
market never traded at.

**Quoting nothing** (`--strategy null`): zero fills, zero P&L, zero residual.

There is also a unit test for the analytic anchor: a taker lifting a two-cent
spread must show an edge of exactly −1.0000 cents per share, and if it does not,
the ledger's sign convention or its mid arithmetic is wrong and nothing else it
reports can be trusted.

---

## 6. Adverse selection on this feed

```
python3 python/analysis/markout.py docs/figures/touch-maker.json \
    --svg docs/figures/markout.svg
```

| model | edge c/sh | 100 ms | 1 s | 10 s | unresolved |
|---|---:|---:|---:|---:|---:|
| naive | 1.3515 | +0.1212 | +0.1765 | +0.1712 | 519 |
| optimistic | 1.0162 | +0.0348 | +0.0947 | +0.0655 | 234 |
| mbo | 1.0079 | +0.0360 | +0.0948 | +0.0635 | 217 |
| pessimistic | 1.0021 | +0.0331 | +0.1087 | +0.0678 | 220 |

**The drift is positive, which means this feed does not exhibit adverse
selection at all.** That is a property of the generator, not a finding about
market making. Its centre is a driftless random walk and its levels refill after
a sweep, so the mid tends to come back to where it was; a maker filled during
the sweep is marked at the bottom of it and then sees the price recover.

The right conclusion is narrow: the markout machinery is wired correctly — the
toxic feed in section 3 returns exactly −2.0000 cents at every horizon on demand
— and this particular synthetic price process is not toxic. What real data does
is an open question until real data is run.

Note the `unresolved` column: fills with no ten-second future left in the data.
They are counted rather than dropped, because silently excluding them biases the
long horizon toward whatever the middle of the session looked like.

---

## 7. What this does and does not establish

Holds regardless of the data:

* The four models are ordered and the band is a bound. 200/200 real orders fell
  inside [pessimistic, optimistic], and `mbo` reproduced all 200 exactly.
* Naive over-fills and never under-fills — 139 of 200 over, 0 under.
* The ledger's arithmetic is exact where an exact answer exists: −1.0000 cents
  of half-spread for a taker, +1.0000 edge and −2.0000 drift on the toxic feed,
  gross P&L of exactly zero on a round trip at the same price.
* Latency changes fills in both directions and the harness measures both.
* A position limit is enforced per lane and never breached.

Does **not** hold, and needs real data:

* Every P&L number in section 1. They are the output of a synthetic price
  process and they measure the generator as much as the strategy.
* The markout signs in section 6, for the reason given there.
* Whether a real day's cancels sit nearer the optimistic or the pessimistic
  bound. `--cancel-front-bias` is the knob that decides this on synthetic data,
  and its real value is an empirical question that one day of MSFT would answer.

Known modelling limits, recorded here rather than in a footnote:

* A replace resets the ahead-count from the book, which breaks the induction
  the optimistic model relies on when an iceberg refreshes.
* Two of our own orders at one price make the clamp limit model-dependent.
* Hidden prints are treated as consuming nothing, which is right for real
  non-displayed liquidity and wrong if a venue's print attribution differs.
* With a position limit in force the four lanes no longer send identical
  orders: a lane that filled more suppresses more quotes. The comparison
  becomes "one strategy, four fill models, each with its own risk state",
  which is the honest version of a market-making backtest and a different
  experiment from the unlimited one.

---

## 8. Reproducing all of it

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
python3 python/make_queue_feed.py data/raw/queue_long.gz --messages 200000 \
    --gap-ns 300000 --seed 7

./build/queue_backtest data/raw/queue_long.gz --strategy touch-maker \
    --max-position 1000 --json docs/figures/touch-maker.json
python3 python/analysis/fill_comparison.py docs/figures/touch-maker.json \
    --svg docs/figures/fills-total.svg
python3 python/analysis/fill_comparison.py docs/figures/touch-maker.json \
    --metric shares --svg docs/figures/fills-shares.svg
python3 python/analysis/markout.py docs/figures/touch-maker.json \
    --svg docs/figures/markout.svg

./build/latency_sweep data/raw/queue_long.gz --strategy touch-maker \
    --max-position 1000 --csv docs/figures/latency-touch-maker.csv
python3 python/analysis/latency_sweep.py docs/figures/latency-touch-maker.csv \
    --svg docs/figures/latency-pnl.svg --shares-svg docs/figures/latency-shares.svg

python3 python/analysis/leave_one_out.py data/raw/queue_long.gz \
    --binary build/queue_sim --samples 200 --partial-only
```

CI runs the closed-form check and the leave-one-out oracle on every push, so
neither can rot silently.

### On a real day

Everything above is synthetic. `scripts/real-data-run.sh` runs the same four
measurements against a real NASDAQ day and writes the tables and figures to
`out/real/`:

```bash
./scripts/real-data-run.sh 12302019.NASDAQ_ITCH50.gz MSFT 200
```

Two things it does that are easy to forget by hand. It **slices first** —
`queue_backtest` does not filter by symbol, so pointing it at a full day would
interleave eight thousand symbols into one book and produce confident nonsense.
And it builds **Release** — the default Debug build has ASan and UBSan on and
runs about ten times slower, which on 1.2M messages is the difference between a
coffee and an afternoon.

The last argument is the leave-one-out sample count. Each sample replays the
whole slice once, so 200 is worth doing and is not a thirty-second job; start
at 50.

Nothing under `data/` or `out/` is committed. Both are gitignored, because both
are derived from licensed data.
