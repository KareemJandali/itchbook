# Phase 6 — Queue-Position-Aware Backtester: Design

**Status:** BUILT. This document is the design as written *before* the phase was
implemented, kept unedited as the record of what was intended — the results and
what actually happened are in
[`docs/phase6-results.md`](phase6-results.md). Written after five independent
designs and five adversarial critiques; section 11 records the errors the
critiques caught so they are not reintroduced.

Two things in it were **not** built, and section 9 says which and why. Reading
this as a description of the shipped code will mislead you in those two places;
everything else shipped substantially as designed.

**Deliverables:** `include/itchbook/sim/*`, `tools/queue_backtest.cpp`,
`tools/latency_sweep.cpp`, `python/analysis/fill_comparison.py`, `docs/phase6-results.md`.

---

## 1. What Phase 6 is, and the one problem it exists to solve

Phases 1–5 replay someone else's market and then run our own matching engine
beside it. Phase 6 places *our* passive order into *their* book and asks what it
would have been filled. The build plan states the problem:

> A public market data feed tells you a cancel happened at your price level. It
> does not tell you whether that cancel was **ahead of** or **behind** your order.
> Ahead means you moved up the queue. Behind means you didn't.

Everything in this phase follows from one asymmetry:

* An **execution** at a price level is, under price-time priority, taken from the
  **front** of the FIFO. It advances you in every honest model. No ambiguity.
* A **cancel** names an order at an unknown position. It is the only ambiguous
  event, and it is where the three fill models differ.

So the three required models differ in exactly one boolean — *was this cancel
ahead of us?* — and share every other line of code. That is what makes the P&L
gap attributable to the queue assumption and to nothing else.

### 1.1 The honest qualification, stated up front

TotalView-ITCH 5.0 is **order-by-order (MBO)**, not aggregated depth (MBP). Every
`E`/`C`/`X`/`D`/`U` names an order reference. For any reference we watched arrive,
whether it was resting at our price *before* we joined is a **fact**, not a guess.

So the optimistic/pessimistic bracket is not forced by *this* data. It is forced
by *aggregated* data — MBP-10, most non-NASDAQ venues, most vendor feeds — and it
bounds a residual model error (hidden interest, display-price sliding, non-displayed
ranked prices) that reference identity provably cannot see.

Shipping the bracket while sitting on MBO data, without saying so, is the one
intellectually dishonest move available in this project, and the first practitioner
who reads the write-up will catch it. So we ship a **fourth model, `mbo`**, that
resolves ahead-vs-behind by reference identity, and we lead with:

> Here is the band a depth-feed backtest forces on you. Here is the reference-resolved
> answer inside it. Here is how far the band would have misled you, and here is the
> rate at which even the reference-resolved answer is contradicted by the tape.

The three required models remain the deliverable. `mbo` is a diagnostic and an
internal oracle: the bounds must bracket it, per order, per event.

**It is `mbo`, not `exact`.** It is exact with respect to *displayed* liquidity on
*one venue* only. Naming it `exact` invites exactly the criticism it exists to
preempt.

### 1.2 Structural premise: our orders never enter `book::Book`

The feed book stays a byte-faithful mirror of the ITCH stream. Our simulated orders
live in a side table keyed by `(side, price)`. This is load-bearing in four ways:

1. Phase 3's done-condition is that the C++ book is bit-identical to the Python
   oracle over a full day. A phantom order changes `shares_at()`, `top()`,
   `resting_shares()` and `best_bid()/best_ask()` and silently breaks it — and the
   differential driver would not catch it, because it never places orders.
2. Feed `E`/`C`/`X`/`D`/`U` address orders by ITCH reference, so **no feed message
   can ever remove one of our orders**. An order injected into the feed book is
   immortal for the rest of the day: it inflates `Level::shares`, can become the
   touch via `Side::level_filled` and stay there, and by mid-session the touch is a
   wall of our own zombie quotes.
3. `Matcher::rest()` calls `book_.add(m.req.id, ...)`, so our ids would share the
   `RefMap` namespace with real ITCH references, and `RefMap::insert` resolves a
   duplicate by last-writer-wins — overwriting the slot pointer and orphaning the
   previous `Order` inside its `Level`, silently corrupting `Level::shares` and
   `count`.
4. Any taking path that calls `Book::take(ref, qty)` on a real ITCH order deletes it
   from the replay; the feed's later `E`/`X`/`D` for that reference then bumps
   `Book::unknown_ref()`. The validated MSFT day had **zero** unknown references,
   which is precisely why the model can be trusted on it. Destroying that signal
   with our own trades is self-inflicted.

Consequence: `engine::Matcher` **is not used for anything that touches the feed
book.** `Matcher::match()` breaks out of its loop the instant it meets a resting
order it has no `Meta` for (`matcher.hpp:302-304`, `if (mit == orders_.end()) break;
// not ours; cannot happen in-engine`), which is *every* order the feed put in the
book. Against a feed-populated book `Matcher::submit()` trades zero shares, always.
Phase 6 gets its own ~60-line taking path (`sim/taker.hpp`) that computes a fill
schedule against a **scratch overlay** and never mutates the feed book.

### 1.3 What Phase 6 produces

* The headline chart: same strategy, same order tape, **four** fill models, P&L per
  filled share, with the naive→pessimistic gap as the result.
* A latency sensitivity curve: P&L vs one-way latency N on a log grid.
* Adverse-selection markouts at 100 ms / 1 s / 10 s, per model, share-weighted, with
  block-bootstrap confidence intervals.
* A fee-aware P&L with exchange, rebate and **regulatory** costs separated, and a
  break-even maker rate per model.
* A knowingly unprofitable strategy that is proven to lose money.

---

## 2. The queue model — message-by-message

### 2.1 State

One `Entry` per live order of ours, per model:

| field | meaning |
|---|---|
| `side`, `price` | our resting `(S, P)` |
| `display` | our currently **displayed** slice (an iceberg shows less than it holds) |
| `hidden` | iceberg reserve, not in any queue |
| `ahead` | believed **displayed shares in front of us** at `(S, P)`. THE state variable. |
| `ahead0` | `ahead` at arrival, for reporting |
| `arrived_ns` | arrival timestamp — never the decision timestamp |
| `ahead_set` | `mbo` only: a small open-addressed map `ref -> recorded shares`, built once at arrival |

Helpers, defined once and used by every fill decision so a sign error cannot hide:

```cpp
// For a resting order on side S at limit P.
inline bool at_or_through(Side s, int32_t limit, int32_t px) {
    return s == Side::Buy ? px <= limit : px >= limit;
}
inline bool better_than_ours(Side s, int32_t limit, int32_t px) {
    return s == Side::Buy ? px > limit : px < limit;
}
```

### 2.2 Two-phase resolution: pre-mutation, apply, post-mutation

`E`, `C`, `X`, `D` and `U` carry **only an order reference**. The affected order's
side, resting price and remaining shares exist only in the pre-mutation book — for a
`D`, or an `E` that zeroes the order, they are unrecoverable afterwards. The clamp,
conversely, needs the post-mutation level size. Neither phase alone is enough:

```
1. Resolved r = sim::resolve(book, type, payload);   // BEFORE the mutation
2. book::apply(book, type, payload);                 // the market moves
3. qm.commit(r, book);                               // AFTER: fills, advance, clamp
```

A convenience `qm.step(book, type, p)` does all three so a driver cannot get the
order wrong. `resolve()` is fused into `book::apply_ex()` (§8) so the ref map is
probed **once**, not three times: `book.hpp` exposes `RefMap::find_index` precisely
so callers do not walk the chain twice, and `E/C/X/D/U` are 52% of a real feed.

Two prices, not one. A `C` has a *resting* price (the level the shares leave, which
is what queue accounting keys on) and a *stated print* price (what naive fills at,
and what enters volume/VWAP). Conflating them is the mechanism behind two of the
fatal bugs in §11.

### 2.3 The two arithmetic rules

**Trade-class — consume then spill.** `ahead` is read **before** the decrement:

```
used     = min(removed, ahead)
ahead   -= used
overflow = removed - used
fill     = min(overflow, display)          // capped at the DISPLAYED slice
if (fill > 0) { display -= fill; ahead = 0; }
```

**Cancel-class — consume and discard.** Never spills into a fill:

```
if (from_ahead) ahead -= min(removed, ahead)
```

Applying one uniform "consume the queue, then fill from the remainder" path to
every advancing event is the single most likely double-count bug in the phase: it
fills you off a large `D` at your level. The two rules are separate on purpose.

Every subtraction compares before subtracting. `removed - ahead` evaluated the
other way round wraps to ~4e9 and reports a fill of the entire order. `Book::reduce`
already documents this trap; the queue model has five more instances of it.

### 2.4 The clamp

After every message that touched our `(S, P)`:

```
limit = post.shares_at(S, P) + own_live_shares_ahead(e)
if (ahead > limit) { ahead = limit; ++clamp_events; clamp_shares += ahead - limit; }
```

`own_live_shares_ahead(e)` sums the live **displayed** quantity of *our own* other
entries at the same `(S, P)` that arrived strictly before `e`. Our orders are not in
`book::Book`, so `shares_at()` cannot see them; clamping to the raw level total
silently deletes our own earlier orders from `ahead` and double-fills a two-order
quoting strategy (§11, trap 3).

Sound because the true shares ahead of us are a subset of `limit`, so `truth <= limit`
always: the clamp can only remove over-estimates, never make a model more optimistic
than the truth. It also makes "the level emptied" need no code of its own — `limit = 0`
forces `ahead = 0` and we are at the front.

**But it changes what "pessimistic" means, and the write-up must say so.**
Clamped-pessimistic is not "cancels never advance you"; it is "you advance only as
fast as executions and the level's shallowest moment allow." Since `D` is 44.95% of
the feed, the clamp fires constantly and is the largest single free parameter in the
phase. Therefore: **on by default** (an unclamped pessimistic claims thousands of
shares are ahead of us at a provably empty level — that is not a bound, it is a bug),
**and** `pessimistic_unclamped` is reported as a fifth diagnostic curve with
`clamp_events` / `clamp_shares` beside it, so the reader can see exactly how much
work the clamp is doing.

### 2.5 Price priority — the fills the queue models cannot see on their own

A rule that only fills on same-side `E`/`C` at our exact price deletes the single
most important passive fill in existence: **the one where the other side comes to
you.** Our order is not in `book::Book`, so the feed can legally rest an ask at or
below our phantom bid, and `Book::crossed()` is false because we are not there. In
reality an incoming sell that locks or crosses our resting bid takes us out
instantly, at our price, in full. There is no queue ambiguity: **price** priority
decides it, not time priority.

This is worst exactly where it matters most. An order that improves the touch has
`ahead == 0` and, by construction, *no real order at its level* — so no `E`/`C` can
ever name it, and it would never fill at all. Every omitted fill is an
adverse-selection fill, so the bias is one-directional and flatters P&L.

Three triggers, applied **identically in all four models** (there is nothing to
model), evaluated on the post-mutation book:

* **R1 — lock onset.** The first message after which the opposite touch is
  at-or-through `P`. `reach` = total opposite-side displayed shares at prices
  at-or-through `P` (walk `Book::top(opposite, n)` while `at_or_through`). Consume
  `reach` under the trade-class rule. Trigger `Lock`.
* **R2 — lock persistence.** While the lock stands, each opposite-side `A`/`F`/`U`-add
  at a price at-or-through `P` consumes `add_shares` under the trade-class rule.
  R1 consumes standing depth once; R2 consumes newly arriving depth. They do not
  double-count.
* **R3 — through print.** A printable trade (`E`, printable `C` at its print price,
  or `P`) at a price **strictly through** `P` on our own side. Under price priority a
  displayed order at `P` would have traded first. Consume `print_shares` under the
  trade-class rule. Suppressed while R1/R2 are active so the two mechanisms cannot
  both claim the same flow. Trigger `Through`.

R3 is what catches a **sweep that walks past our level**: the aggressor eats the bid
queue at `P`, empties it, and continues at `P-1`. Without R3, our fill is capped at
our level's real depth when in the counterfactual we were at the front of that level
and the aggressor would have taken us before ever reaching the next price. The
direction of that omission is, again, the adverse-selection direction.

Fills from R1/R2/R3 are **maker** fills at our own price: the counterparty came to us.

`lock_shares`, `through_fills`, `through_shares` and `lock_dwell_ns` are all
reported. A run in which they dominate is a run in which the queue assumption is not
what is being measured, and the reader must be able to see that.

### 2.6 The table

`S` = our side, `P` = our limit. "Ambiguous" is the cancel class: optimistic advances,
pessimistic does not, `mbo` tests reference membership, and **none of them fill**.

| Msg | Resolved effect | naive | optimistic | pessimistic | mbo |
|---|---|---|---|---|---|
| `A`/`F` at our `(S,P)` | joins the **back** | — | `ahead` unchanged | unchanged | unchanged; ref recorded as post-arrival |
| `A`/`F` on our side, better price | new level in front | — | no effect | no effect | no effect |
| `A`/`F` on our side, worse price | — | — | no effect | no effect | no effect |
| `A`/`F`/`U`-add opposite, at-or-through `P` | locks/crosses us | **R1/R2 fill** | **R1/R2 fill** | **R1/R2 fill** | **R1/R2 fill** |
| `A`/`F` opposite, not at-or-through | — | — | no effect | no effect | no effect |
| `E` at our `(S,P)` | trade, front of FIFO | fill `min(shares, display)` | trade-class advance + fill | identical | identical; decrement the set entry if present |
| `E` on our side, price strictly through `P` | sweep walked past us | fill | **R3 fill** | **R3 fill** | **R3 fill** |
| `E` on our side, price better than `P` | flow went in front | — | no effect | no effect | no effect |
| `E` opposite side | — | fill iff at-or-through | no effect (R1 may fire) | same | same |
| `C` printable, print price **==** resting price, our level | trade | fill | trade-class + fill | identical | identical |
| `C` printable, print price **!=** resting price | matched under a regime we do not model | fill iff `at_or_through(print)` | **ambiguous** | **ambiguous** | **ambiguous** |
| `C` **non-printable**, any price | reported elsewhere as an aggregate `Q` | **never fills** | **ambiguous** | **ambiguous** | **ambiguous** |
| `X` at our `(S,P)` | partial cancel; order keeps its place | — | `ahead -= min(rm, ahead)` | no change | membership |
| `D` at our `(S,P)` | full withdrawal | — | `ahead -= min(rm, ahead)` | no change | membership |
| `U`-delete at our `(S,P)` | original leaves entirely | — | `ahead -= min(rm, ahead)` | no change | membership |
| `U`-add at our `(S,P)` | joins the **back** | — | unchanged | unchanged | unchanged; new ref recorded post-arrival even if the original was ahead |
| `U` wholly inside our level | delete then add | — | full decrement | **complete no-op** | decrement iff original was ahead, then record the new ref behind |
| `P` at a price better than `P` | hidden interest in front took the flow | — | no effect (`hidden_ahead_shares`) | same | same |
| `P` at `P` | aggressor bypassed our level | fill (default on) | no effect (`hidden_at_price_shares`) | same | same |
| `P` at a sub-tick price inside the spread | midpoint peg | fill iff at-or-through | no effect (`hidden_inside_shares`) | same | same |
| `P` strictly through `P` | price-priority impossible | fill | **R3 fill** | **R3 fill** | **R3 fill** |
| `Q` | never touches the book | **never fills** | never fills | never fills | never fills |
| `H` | no queue effect; `ahead` survives a halt | fills suspended | suspended | suspended | suspended |
| `S` | session gate | gates all four identically | " | " | " |
| unknown ref | side and price unknowable | counted, not guessed | counted | counted | counted; run flagged approximate |

### 2.7 The rules behind the table

**One `C` rule to remember.** *A `C` fills only when `printable == 'Y'` **and**
`print_price == resting_price`. Every other `C` is cancel-class.* This single rule
closes both of the phase's most dangerous silent failures: a non-printable `C` at the
cross price would otherwise fill every queue model in the closing auction, and a
repriced `C` would otherwise manufacture a fill under a matching regime the model
does not describe. On the validated day 2,500,408 of 6,154,278 shares — **40.6%** —
executed at exactly two prices in the two crosses, and cross participants' displayed
orders are removed by exactly this message shape.

**Relevance is decided by the resting price, never by `C`'s wire price.** A `C`
printing at 100.02 against an order resting at 100.00 is an event at our level if we
are at 100.00, and is not one if we are at 100.02.

**`P` never advances any queue model.** Those shares were never in the displayed
FIFO. And the direction people assume is backwards: on NASDAQ continuous priority is
**price, then display, then time**, so non-displayed size at our own price sits
**behind** us and does not delay our fill at all. What hidden liquidity actually
costs us is *flow interception* — a hidden order at a better price, typically a
midpoint peg inside a one-tick spread, takes marketable flow before it ever reaches
our level. That is why the three hidden buckets are split by price relative to our
limit, and why `hidden_inside_shares` is the one worth publishing.

**Never look up `P`'s order reference in the RefMap.** It is not a book reference;
probing it inflates `Book::unknown_ref()`, which is the run's data-quality canary.

**Never key on `P`'s Buy/Sell indicator.** It is the side of the non-displayed order,
and in TotalView output it is widely observed to be constant `'B'` — both of this
repo's generators hardcode it (`gen.trade(t, ref, b"B", ...)` in `make_bench_feed.py`
and `fuzz_feed.py:187`). Census the field on a real day and publish the split before
any rule depends on it.

**`U` decomposes into delete-then-add, in that order,** mirroring `Book::replace` and
`book.py`. The delete half inherits all of `D`'s ambiguity; the add half joins the
back with certainty. `U` is 4.49% of the validated day and `D` is 44.95%, so together
they are ~49.4% of the feed and are the entire source of the optimistic/pessimistic
spread. `X` — the message people think of as "the ambiguous cancel" — is 0.07%. The
spread is a `D`-and-`U` phenomenon and the write-up should say so. `U` with
`new_ref == original_ref` is legal on the wire and must not be shortcut as a no-op:
it is still delete-then-add and the order goes to the back.

The `X`/`U` distinction is about which order-entry message the participant chose, not
about their intent: **`X` ⇒ priority retained; `U` ⇒ priority lost.** Our own order
API mirrors the matching rule, not an inferred one-to-one message correspondence:
`amend_down()` keeps `ahead`, `replace()` resets it.

**`ahead` survives everything except our own actions.** It is a property of one
`(S, P)` FIFO, mutated only by messages at that exact side and price. Nothing about
`best_bid`/`best_ask`, a better level appearing in front of us, that level emptying,
or our level going empty and refilling ever resets it. Re-deriving `ahead` from the
book on a touch change would throw away the exact edge the phase exists to measure —
45% of the feed is `D`, so a level that jumped in front of us very often just cancels
away, and we return to the touch with the position we earned minutes ago. The
one-directional rule (§9, **I2**) makes this testable rather than a claim.

**A partial fill of our own order leaves us at the front.** `display -= fill`, `ahead`
stays 0, priority preserved. Never re-derive `ahead` from the book after a fill: the
book still holds real orders that our counterfactual already consumed, so re-deriving
would re-insert us behind liquidity we just traded through, making a partial fill
strictly worse than no fill at all.

**Icebergs.** The fill is capped at `display`, never at `display + hidden`. When
`display` reaches 0 with `hidden > 0`, the model refreshes its own slice internally —
`slice = min(hidden, req.display)`, `ahead = shares_at(S,P) + own_live_shares_ahead` —
i.e. straight to the back. `QueueModel` owns its slices; it does not call into
`Matcher` (whose `refresh_iceberg(Meta&)` is private and driven from the maker-side
fill path). This is where `matcher.hpp`'s comment that hiding size costs queue
position becomes a number.

**Session and halt gating is hoisted above the model dispatch,** so all four models
see byte-identical eligibility. Non-tradable trading states are `'H'` (halted),
`'P'` (paused) and `'Q'` (quotation only — the state a symbol sits in while a
halt-resumption cross executes mid-session). Only `'T'` is tradable, and
`trading_state_` initialises to `'\0'` in `book.hpp` and is only ever set by an `'H'`
message, so `'\0'` is *unknown-and-not-tradable* and is counted separately, never
treated as trading. `'H'` carries a stock field and must be locate-filtered.

Watch the three-way collision on the letter `Q`: system-event code `'Q'` (start of
market hours), trading-state code `'Q'` (quotation only), and message type `'Q'`
(cross trade). They are unrelated and all three matter here.

**The session gate does not keep you out of the opening cross.** NASDAQ sends
`S = 'Q'` at 09:30:00 and the opening cross prints immediately after it, strictly
inside the gate. The non-printable-`C` rule is what keeps us out; the session gate
is not.

**`CrossPolicy::Exclude` is the default and it is not conservative.** It removes 40.6%
of the validated day's volume from the opportunity set, and the sign of that
exclusion is arbitrary: a passive bid that would have filled in a closing cross below
the last mid is spared a loss; above it, denied a gain. The results doc prints the
continuous-displayed share of volume —
`6,154,278 − 2,500,408 − 568,546 = 3,085,324`, **50.1%** — as the fraction of the
tape the four models actually address. `CrossPolicy::FillAtCrossPrice` exists as a
flag but is not the default: it needs the imbalance messages (`I`/`N`) we do not
parse, and without them it would be grading its own homework.

**Unknown references are counted, never guessed.** Do not gate on "would it touch our
level" — `D`, `X` and `E` carry no price on the wire, so that predicate is
uncomputable for 99.9% of the references it would need to be evaluated for. Count
every unknown reference; if the count is non-zero the run is labelled approximate,
`mbo` is labelled approximate, and `--strict` (the default for publishable runs)
refuses to write the JSON.

### 2.8 The `mbo` model, precisely

At arrival, walk the level's FIFO once via the new `Book::first_order(side, price)`
accessor and record `ref -> shares` for every order then resting there, into a small
open-addressed map sized to the level's `count` (tens to hundreds of entries).
Thereafter:

* **Cancel-class events** use set membership: `from_ahead` iff the reference is in the
  set. On a **partial** removal, decrement the recorded shares and leave the entry in
  place; erase only when the removal is total. Erasing on any hit drops the rest of
  that order out of `ahead` on the next event and makes `mbo` fill early.
* **Trade-class events advance unconditionally, exactly as pessimistic does.** An
  execution at our price is unambiguous evidence that displayed liquidity at our
  price traded. This is what makes the containment invariant true by construction:
  `mbo`'s advance set is pessimistic's plus a subset of optimistic's cancels, so
  `ahead_opt <= ahead_mbo <= ahead_pess` pointwise.
* When a trade names a reference **not** in the set while `ahead > 0`, that is a
  **priority anomaly** relative to the displayed FIFO — display-price sliding
  (ranked at a price other than the displayed one), non-displayed ranked interest, or
  routed-away-and-returned orders. `mbo` still advances; it increments
  `priority_anomalies` and accumulates the share weight. **The share-weighted anomaly
  rate is the honest measure of how far `mbo` is from truth**, and the aggregated
  models cannot even detect it.
* Membership is O(1). Never a `std::vector` scan — `level.hpp` already warns that
  `Level::shares_ahead_of()` is O(n) and must not go on the hot path, and a linear
  membership test reintroduces exactly that.
* Because trade-class events advance the scalar without a set entry to decrement,
  `ahead` and the set sum can drift. `ahead` is authoritative; the set is only the
  cancel predicate. Invariant **I3b** (§9) states the equality conditionally.

---

## 3. The three fill models, as precise rules

All four models share: the same session/halt gate, the same arrival timestamps, the
same `Entry` lifecycle, the same price-priority triggers R1/R2/R3, and the same
`min(overflow, display)` cap. They differ only where stated.

### 3.1 `naive` — touch fill

> On any **printable** trade at a price at-or-through our limit, fill
> `min(printed_shares, display)`.

* `ahead` is not tracked at all (equivalently, `ahead ≡ 0`).
* Fills on `E`, on printable `C` (at its **print** price), and — by default — on `P`.
* **Never** on `Q`. A closing cross is millions of shares at a single price; a naive
  model that fills on every print at-or-through its limit consumes its entire order
  on that one message and the headline chart becomes a measurement of the auction.
  This is the highest-risk silent failure in the phase.
* **Never** on a non-printable `C` — by definition already reported elsewhere as an
  aggregate, so filling would double-count against that print.
* `min(printed_shares, display)`, **not** the full order. "Fill whenever the market
  trades at your price" taken literally lets a 500-share quote fill in full against a
  100-share print, inventing 400 shares that never traded. Then naive is not an upper
  bound on anything and invariant **I5** collapses.
* With two of our own orders resting at one price, each print is allocated
  **oldest-first** across them up to the print size — otherwise naive fills 200 shares
  against a 100-share trade and violates **I7**. The reference strategy keeps one live
  order per `(side, price)` and asserts it.
* **Naive respects arrival time.** It is "no queue", not "no clock". A model
  insensitive to arrival is filling on trades that happened before the order reached
  the exchange, which is straightforward look-ahead. Consequence: the predicted shape
  of the latency curve is that naive **also decays**, just far more slowly than the
  queue models — not that it is flat.
* Filling on `P` is the faithful strawman (a naive backtester working off a trade tape
  fills on hidden prints), but it inflates the naive curve, which is the direction
  that makes the project look good. So it is a flag, `--naive-hidden` (default on),
  and `naive_fills_from_hidden` / `naive_shares_from_hidden` are **mandatory** report
  fields. The results doc shows naive both ways.

### 3.2 `optimistic` — every cancel at our level was ahead of us

> Cancel-class events at our `(S, P)` decrement `ahead` by `min(removed, ahead)`.
> Trade-class events advance and may fill. Cancels never fill.

`from_ahead = true` for `X`, `D`, `U`-delete, repriced `C` and non-printable `C`.

### 3.3 `pessimistic` — only executions advance you

> Cancel-class events at our `(S, P)` do nothing. Trade-class events advance and may
> fill.

`from_ahead = false` for every cancel-class event. The clamp (§2.4) still applies, so
this is *clamped* pessimistic; `pessimistic_unclamped` is reported beside it.

### 3.4 `mbo` — reference-resolved (diagnostic)

> `from_ahead` for a cancel-class event iff the reference was recorded at arrival.
> Trade-class events advance unconditionally, as pessimistic does.

See §2.8.

### 3.5 The one line that is the whole model

```cpp
bool from_ahead;
switch (cfg_.model) {
    case Model::Naive:       return naive(e, r);                    // separate path
    case Model::Optimistic:  from_ahead = true;                     break;
    case Model::Pessimistic: from_ahead = by_trade;                 break;  // <-- the difference
    case Model::Mbo:         from_ahead = by_trade || in_ahead_set(e, r.ref); break;
}
```

### 3.6 Comparability: two modes, both published

**Mode B — frozen decisions (the headline).** One reference replay records an *intent
tape* (id, kind, side, price, qty, event index, decision timestamp). All four models
are then evaluated against that identical tape, so the fill model is the **only**
variable. This is the correct experimental design for the claim the phase makes, it
is what makes **I5** checkable event-for-event, and it is the only mode in which
`[pessimistic, naive]` is a genuine bracket.

**Mode A — free-running.** Each model drives its own strategy instance over the same
feed, so fills feed back into decisions. This is the truthful end-to-end number and
it is what a strategy would actually experience — but the difference between the four
P&L numbers is then a compound of the queue assumption and every downstream decision
it changed. Mode A reports the **event index at which the four lanes' working-order
sets first diverge** and the fraction of the day after that point. Without that
number beside it, "same strategy, three fill models" is overstated.

Both modes run in **one process, one pass, one shared `book::Book`**, with four lanes
fanned out from a single `resolve()`. "Same data" is then a structural fact, not a
claim, and the containment invariants are checked per event (~4.6M times on the MSFT
day) rather than once on end-of-day totals. This is only sound because our orders
never enter the book; the moment market impact is modelled, the four worlds fork and
need four books.

`Recorder<S>` (records a tape) and `TapePlayer` (replays one) both satisfy the
`Strategy` concept — which is the payoff for making the interface a template
parameter rather than a base class.

The tape carries intents but **not** risk state, so risk limits must be enforced by
the **harness**, not inside strategy code (§6.4). Otherwise, replaying the pessimistic
lane's tape into the naive lane gives the naive lane far more fills with none of the
cap-driven cancels, and its inventory grows without bound — reintroducing exactly the
Δinventory artefact the flatten policy exists to remove.

---

## 4. Latency model

`include/itchbook/sim/latency_model.hpp`. All arithmetic is on the **exchange clock**
— nanoseconds since midnight Eastern, the same clock as `itch::timestamp()`. Latency
is always **added** to an event time, never subtracted from "now".

### 4.1 Five channels

| channel | path | what it decides |
|---|---|---|
| `feed` | matching engine → our decision loop (multicast, NIC, kernel, parse) | how **stale** the state we decided on was |
| `think` | our own decision cost | — |
| `order` | our NIC → matching engine (a one-to-one session through a risk gateway) | where we land in the FIFO |
| `cancel` | same wire, its own knob | the cancel-vs-fill race |
| `ack` | exchange → us (private response) | how long the strategy believes something false. **Affects nothing at the exchange.** |

Each channel has its own RNG stream derived from `(master_seed, channel_index)`, so a
sweep over `order` does not reshuffle `feed` draws. Otherwise the difference between
two grid points is part latency and part noise, and readers will over-interpret the
wiggle. The sweep default is `Shape::Constant` — no RNG at all.

`LatencyModel::symmetric(n)` collapses to the build plan's single N.
`reaction_time() = feed + think + order` — the interval from the exchange generating
an event to our reply hitting the matching engine. It is **not** "tick to trade",
which in the trade means the in-NIC-to-out-NIC time of your own box.

### 4.2 Corrected: the feed/order split does not move queue position

Work the timeline. A message stamped `T` is applied to the exchange book at `T`,
delivered to the strategy's view at `T + feed`, `on_event` fires with
`now = T + feed`, and the resulting action arrives at `T + feed + think + order`.

Two facts follow, and both contradict the intuition:

1. Arrival time depends on the **sum only**, so where we land in the FIFO is
   bit-identical across every split. "`order` alone decides queue position" is false;
   the sum decides it.
2. The state we decided on is the view as of exchange time `T` for **every** split,
   because delivery is triggered by that message. Staleness at decision is always
   exactly `feed`.

So the split sweep is **flat by construction for an event-driven strategy**. It bites
only for timer-driven wakeups (where the strategy's clock decouples from the exchange
clock) and via `cancel` differing from `order`. We keep the axis for timer strategies
and, better, we use "**axis B must be flat for the event-driven reference strategy**"
as a determinism test.

### 4.3 Two books, one clock

The strategy's view cannot be produced by rewinding — a book is not reversible — so it
is a **second `book::Book`** fed the same messages `feed` nanoseconds later, with a
deque of fixed-size reused payload slots between them.

Because our orders never enter either book, the zero-latency equivalence test is
valid for **any** strategy, not just a null one: with every channel at zero, the two
books must emit byte-identical snapshot CSVs, gradeable with the existing
`python/analysis/book_diff.py`. That is the strongest test in this file and it is
free (**I11**).

The clock advances only when a market message arrives. That is correct rather than a
shortcut: nothing in the book can change between messages, so processing a 50 µs-old
arrival lazily just before the next message is observationally identical to
processing it on time.

### 4.4 The time barrier — atomic exchange events

**Feed messages sharing a timestamp are one atomic exchange event.** One aggressive
order sweeping three price levels emits three `E` messages at the same nanosecond. A
barrier that only guarantees "your intent cannot take effect until the next
*message*" lets a quote **step out of the middle of a sweep it is already inside** —
no exchange permits this, and it bites hardest at latency 0, which is exactly the
configuration the oracles would be asserted at.

So:

* Queue accounting runs **per message** (pre-mutation resolve, apply, post-mutation
  commit) — it must, because `E`/`X`/`D` need the pre-mutation book.
* Strategy callbacks fire **once per timestamp group**, after every message in the
  group is applied.
* Our arrivals stamped at `T` are applied **after the entire group at `T`**
  (`TiePolicy::MarketFirst`).

Note also that ITCH match numbers do **not** group a sweep: the match number is the
day-unique identifier of one **execution** — the key the Broken Trade message uses to
bust an individual trade — and ITCH carries no field identifying the incoming
aggressor at all. Timestamp contiguity is the available proxy and the document says
so plainly.

### 4.5 Channel FIFO, corrected

```cpp
Nanos Channel::arrival(Nanos sent) {
    Nanos t = saturating_add(sent, draw());
    if (preserve_order_ && t < last_) t = last_;   // NON-decreasing, not last_+1
    last_ = t;
    return t;
}
```

Two corrections against the obvious version:

* **Non-decreasing, not strictly increasing.** A `last_ + 1` clamp on the `feed`
  channel manufactures a burst-dependent delay: `last_` moves forward by at least 1 ns
  per message, so in any burst arriving faster than 1/ns the accumulated clamp grows
  linearly with burst length. At the open a single symbol sees thousands of messages
  inside a few microseconds — tens of microseconds of synthetic delay, larger than the
  first four points of the latency grid, i.e. the regime the sweep exists to resolve.
  It also breaks the zero-latency equivalence test. Two messages in one MoldUDP64
  datagram genuinely do arrive at the same instant, and `EventKey::seq` already
  supplies the tie-break.
* **Order and cancel share one session FIFO.** A TCP order-entry session cannot
  reorder. Giving cancels an independent `last_` reorders them against submits on the
  same session and manufactures a `cancel_ns < order_ns` race that is a modelling
  artefact, not a property of order entry. `cancel.arrival(sent)` is clamped to be no
  earlier than the arrival of any action already enqueued on that session.
  `CancelUnknown` then fires only for a genuinely unknown id (already-terminal, or
  rejected at submit) — which is the useful bucket.

Saturate, never wrap. The truncation cap on `Shape::Lognormal` is mandatory, not
decorative: a `double`-to-`uint64` conversion needs an explicit cast under
`-Wall -Wextra -Wpedantic -Werror` plus UBSan, and an uncapped draw can schedule an
event past end of day.

### 4.6 Deterministic event ordering

Total order on `EventKey{when, cls, seq}`, `cls` ranking
`Market(0) < Action(1) < FeedDelivery(2) < Report(3) < Timer(4)`, and `seq` drawn
from **one shared counter** across all four sources at enqueue time.

**Every queue stores an `EventKey`**, not a bare `Nanos`. A merge that compares bare
timestamps across four deques cannot break the case it was written to handle — an
order arrival and a feed delivery landing on the same nanosecond — because `cls` and
`seq` are not available at the comparison site. `drain_until()` is one loop that
recomputes the minimum key after each event, because delivering a feed event can
enqueue an action whose arrival is still earlier than `t`.

### 4.7 Tie policy, honestly

`TiePolicy::MarketFirst` is the default and it is **a mixed policy, not a bound**:

* pessimistic for a tying `A`/`F` at our price (it goes ahead of us) and for a tying
  execution against our arriving cancel (we get filled first);
* **optimistic** for a tying cancel at our price — applied before our arrival snapshot,
  it makes `ahead` *smaller* and lands us closer to the front.

Report it as mixed, print `tie_events` and `tie_shares`, and offer `OursFirst` to
bound the other way. A partitioned "worst case for every race" policy is not
implementable without reordering the feed against itself inside a nanosecond, which
would break book reconstruction (an `E` must be applied after the `A` that created its
reference). Do not claim one.

### 4.8 Queue position is snapshotted at **arrival**

```cpp
out.ahead_at_arrival = book.shares_at(side, price) + own_live_shares_ahead(e);
out.queue_delta = int64(ahead_at_arrival) - int64(action.ahead_at_decision);
```

`ahead_at_decision` comes from the **delayed view** book (what the strategy could
actually have known) and is kept only so the report can price the flight. Both sides
of the subtraction add `own_live_shares_ahead`, so the difference is not biased by our
own quoted size.

Three failure modes, and they do not cancel out: the level **thickened** in flight
(you are behind more than you measured — under pessimistic that is a step function,
not a proportional error); the level **thinned** (you land closer to the front than you
thought); and the level **no longer exists** because the touch moved a tick, at which
point the decision-time figure is meaningless. Reported: mean and p99 of
`|queue_delta|`, `touch_moved_frac`, `crossed_on_arrival`.

Use `shares_at()` (O(1) off `Level::shares`, maintained incrementally) — never
`Level::shares_ahead_of()`, which is O(orders at level) and would make the whole thing
quadratic on a level holding hundreds of orders.

### 4.9 Cancel-in-flight

A cancel has **no effect at the exchange until it lands**; the order stays fully live
and fillable. There is no soft-cancelled state at an exchange, so any model that stops
filling you the moment you *decide* to cancel is fantasy — and it removes the entire
loss mechanism.

Outcomes, all counted, none an error:

| outcome | meaning |
|---|---|
| `Cancelled` | landed with the whole remainder intact |
| `CancelTooLatePartial` | some filled in flight (`filled_in_flight`) |
| `CancelTooLateFull` | all of it filled; the cancel finds a terminal state. **Expected, not an error** — never assert on it. |
| `CancelUnknown` | genuinely unknown or already-terminal id |

The strategy is not told until `ack` lands, so it keeps believing the order is live.
A cancel-and-replace pair fired back-to-back can therefore leave **both** orders live
at the exchange for one `order` interval — a genuine over-exposure and a genuine
double-fill source, reproduced for free by queueing both actions at their own arrival
times.

Quantify the damage with the markout machinery: record mids at fill+100 ms/1 s/10 s
for too-late fills **separately** from ordinary passive fills. They should be
systematically worse; if they are not, either the cancel signal is noise or the sim is
wrong.

### 4.10 Arrival-marketable orders

The single largest way naive backtests overstate P&L is ignoring that a passive intent
can arrive **marketable** because the touch came to us in flight. Three policies:

* **`CrossPolicy::Take` (default)** — we cross, pay the spread and the **taker** fee on
  an order we meant to earn a rebate on, through the scratch overlay (§5.3). This is
  the faithful model of a plain limit order.
* `CrossPolicy::RejectBack` — post-only cancel-back. This is an **election**, not
  exchange default: a NASDAQ Post-Only order that would lock or cross re-prices, or
  executes when the price improvement exceeds the maker-taker differential. It is also
  *not* the same mechanism as Reg NMS Rule 610 display-price sliding. And it is
  dangerous as a default: TouchMaker joins the touch, so its bid becomes marketable in
  flight only when the offer **fell to or through it** — the single most adverse moment
  for a bid — and rejecting it for free deletes exactly the fills the latency curve
  exists to reveal, with an effect that grows with N.
* `CrossPolicy::SlideOneTick` — re-price one tick inside before submitting.

Whichever is chosen, `crossed_on_arrival` and `post_only_rejects` are reported. There
is no post-only field on `engine::Request`, so this is a pre-submit check against
`best_ask`/`best_bid` in `apply_action`, not something `Matcher::submit()` provides.

### 4.11 Defaults, and why zero is not one

Zero latency on every channel is the strongest single form of silent optimism
available to a backtester: the strategy reacts to the message that moved the market
and wins queue priority over everyone with real wire time. The default is
`LatencyModel::colo_sw()` — feed 5 µs, think 3 µs, order 12 µs, cancel 12 µs, ack 5 µs.
`symmetric(0)` exists as an explicitly labelled unphysical diagnostic, and
`queue_backtest` refuses to write a publishable JSON at `reaction_time() == 0` without
`--allow-zero-latency`.

Named regimes so a result reads "this needs a colo box" and not "this needs 12000":
`colo_fpga()` (1 / 0.3 / 1.5 µs), `colo_sw()` (5 / 3 / 12 µs), `metro()` (250 µs / — /
400 µs), `retail()` (5 ms / — / 8 ms).

`think` should be measured, not guessed. Phase 4's ~63 cycles/msg is the cost of
`parse` + `book::apply` — feed ingestion — and at 3 GHz that is ~21 ns, 150× smaller
than `colo_sw()`'s 3 µs `think`. It is a defensible **floor for the ingestion portion**
of `think`, not `think`. To make the phase-4-to-phase-6 narrative true, measure the
reference strategy's own `on_event` with the same `bench::cycles_begin`/`cycles_end`
harness and feed *that* into `Shape::Empirical`. `bench::calibrate_cycles_per_ns()`
and `bench::tsc_is_invariant()` already exist in `bench/rdtsc.hpp`; `Histogram`
saturates samples at `UINT32_MAX` and requires `finalize()` before `percentile()`.

### 4.12 The sweep

`tools/latency_sweep.cpp` runs the same feed, same strategy, same seed over a log
grid crossed with the four fill models, one self-describing CSV row per cell;
`python/analysis/latency_sweep.py` renders it.

Grid: `0, 1, 2, 5, 10, 20, 50, 100, 250, 500 µs, 1, 2, 5, 10 ms` — chosen to read as
regimes (0 = the impossible baseline every naive backtest assumes; 1–5 µs FPGA/kernel
bypass; 10–50 µs good software colo; 100–500 µs well-connected non-colo; 1–10 ms
retail/cloud). `log(0)` is undefined, so the plotter renders 0 as a separate leftmost
category with an axis break, never as 1 ns.

Axes: **(A) symmetric N** — the headline; **(C) cancel alone** — prices the too-late
losses; **(B) the feed/order split** — flat by construction for the reference strategy
(§4.2), retained for timer strategies and used as a determinism test.

Cost is not a constraint: the book replays at ~44M msg/s and MSFT's day is 1.2M
messages, so a 14-point × 4-model × 20-seed sweep is seconds. Report a **band across
seeds** per grid point, not a single number: one day of one symbol is one sample, and
adjacent grid points can differ by less than day-to-day noise.

"This strategy is only profitable below 20 µs" is the single most useful sentence a
backtest can produce, and it is a sentence the naive touch-fill model is structurally
incapable of saying. The expected shape is the argument: naive decays slowly (only the
crossing and price-priority effects touch it) while the queue models decay faster, and
the cliff is where flight time crosses the level's mean lifetime. Locating that cliff
is the deliverable.

Sanity gate: for the known-unprofitable strategy, confirm the curve is **explicable**
rather than merely monotone. P&L rising with N is legitimately possible when being
slow means missing toxic fills — and in that case the markout columns must show the
avoided fills were toxic. If they do not, the sim is wrong.

---

## 5. Adverse selection and transaction costs

### 5.1 Markout: definition and sign

For a fill of `q` shares at price `F`, with `sgn = +1` for a buy and `-1` for a sell:

```
markout(h) = edge + drift(h)
edge       = sgn * (M(T)     - F)        half-spread captured (or paid)
drift(h)   = sgn * (M(T + h) - M(T))     the pure adverse-selection term
```

**Positive is in our favour, in every column, always.** Negative means we were picked
off. Adverse selection is reported as `-drift`. A `price_impact()` accessor returning
`-drift` is provided for comparison against published Rule 605 numbers, which use the
opposite convention.

The three models are compared on **`drift`**, not on `markout`. And `edge` is **not**
claimed invariant across models — that claim is false and would be falsified by the
first real run. The models fill at *different times* by construction, so the same
resting price `F` meets a different `M(T)`. Worse, because our orders are not in the
book, a resting buy at `F` can fill (via R1/R3) while `F` sits above the prevailing
ask, giving a **negative** edge on a nominally passive fill. So `edge` is reported per
model as a measured quantity, and the gap between models is itself a result: it says
how far the touch had moved before each model filled us.

### 5.2 The mid

`two_mid = best_bid + best_ask`, an `int64`, **never divided** until print time.
Drift and edge are stored doubled (`two_edge`, `two_drift`); conversion to
micro-dollars is `two_x * 50`.

The reason is *not* rounding of a half-cent mid: `Price(4)` has four implied decimals,
so half a cent is 50 `Price(4)` units and is exactly representable, and Reg NMS Rule
612 puts every displayed quote on a $0.01 grid for an NMS stock ≥ $1.00, so
`bid + ask` is always even and the mid is exact. `two_mid` is carried because (a) it
keeps every intermediate an exact integer with no `double` anywhere in the accounting
path, and (b) it stays exact for sub-penny prices — sub-$1 names, midpoint prints,
price-slid `C` ranks — which `book.hpp` already routes to the overflow map. The
`int64` **is** required regardless: the $199,999.99 stub quote is 1,999,999,900 in
`Price(4)` and two of those overflow `int32`.

`MidStatus`: `Ok`, `Locked` (bid == ask, kept — a transient single-venue lock has a
well-defined midpoint), `OneSided`, `StrictlyCrossed` (bid > ask), `Halted`,
`OutsideContinuous`, `Empty`. Only `Ok` and `Locked` are usable.

Note that `Book::crossed()` is `return b >= a;` — it counts a **locked** book as
crossed. That is a documented quirk of the existing API and is correct for the
matcher's invariant, so the sim uses its own strict predicate rather than changing it.

**Gate on session phase first, then on trading state.** `Book::system_event()` must
have passed `'Q'` and not yet reached `'M'`; then `trading_state() == 'T'`. Testing
`trading_state() != 'T'` alone drops 100% of samples until the first `'H'` message
arrives (`trading_state_` initialises to `'\0'`), and testing `== 'H'` alone lets the
entire pre-open (`'Q'`, quotation only) and every paused state (`'P'`) through as
`Ok`. Neither detects the post-close: the real MSFT run ends with `trading_state = T`
and `system_event = M` at 19:00, so the thin after-hours book flows straight into the
`Ok` bucket unless the phase gate is applied.

### 5.3 The prevailing mid — batch start, not previous message

The mid stamped on a fill is the mid **as of the start of the current nanosecond
batch**, cached whenever the timestamp changes and stamped on every fill generated
within that batch.

Not the post-trade mid: the filling message can move the touch by itself, and folding
that into `edge` understates adverse selection by up to a half tick per fill — larger
than the entire rebate. And not the previous *message's* mid either: one aggressive
order sweeping N resting orders emits N `E` messages, and re-observing the mid after
each leg stamps leg k with a mid already walked down by legs 1..k-1. Every leg then
records `edge ≈ +half-spread` against its own already-moved mid, and `drift` starts
from a mid that has already absorbed most of the move — systematically understating
**both** effective spread and adverse selection for exactly the toxic events the phase
exists to measure.

Batch-start is causal (you never need to know the batch is over), it is consistent
with the LOCF definition used at horizon endpoints, and it is what makes `edge`
comparable to Rule 605's order-receipt mid.

### 5.4 Horizons and the resolution mechanism

Define `M(u)` := the mid after applying every message with timestamp `<= u`
(right-continuous, last-observation-carried-forward).

One **FIFO deque per horizon**, drained in stream order — no history storage, no heap.
At the top of the replay loop, before `book::apply()` for the message at `t`, call
`advance(t, mid_prev)`; for each horizon deque, pop while `front.due_ns < t` and
resolve against `mid_prev`. The **strict `<`** is what makes a batch of messages
sharing one nanosecond land on the correct side of a horizon expiring inside it; a
`<=` would resolve against a mid that predates the batch.

Valid because fills arrive in non-decreasing timestamp order and every entry in a
given deque carries the same fixed offset, so each deque is already sorted. It is
exact — bit-identical to storing a full mid tape and binary-searching it, because both
compute the same LOCF step function — and it handles a symbol-filtered replay
correctly by construction: if there are no messages for our symbol for three seconds,
the mid did not change during those three seconds either. `on_fill()` asserts the
horizon list has not changed since the first fill; a per-order horizon would silently
break the sortedness the whole design rests on.

Honest accounting of the memory claim: the deques are O(fills in the longest horizon),
but `samples_` retains one ~40-byte `Sample` per fill × horizon × cohort for the whole
run, and `raw_two_drift` per report cell. That dominates, and it is a deliberate
choice (exact percentiles, matching the `bench/histogram.hpp` precedent). The deque
earns its place by avoiding a heap and a binary search per resolution, not by saving
memory.

Default horizons `{100 ms, 1 s, 10 s}`, `300 s` opt-in for the Rule 605 comparison.
`h == 0` is not a horizon — the fill-instant markout is exactly `edge`, stored **once
per fill** in a table keyed by `fill_seq`, not duplicated into every horizon's sample
(duplicating it makes the `edge` column vary across horizons because each horizon has
a different exclusion set, for a reason that has nothing to do with spread capture).

### 5.5 Exclusion, split into two different decisions

**At the fill instant.** An unusable mid means the fill's whole markout series is
unusable and `edge` is undefined. Drop it from the markout table — that is
contemporaneous information only, so it is a legitimate exclusion. Critically, the
fill must **also** be kept out of the ledger's attribution accumulators: `observe()`
returns `two_mid = 0` for `OneSided`/`Empty`, and letting that reach
`sum_sgn_q_two_mid_` mis-attributes the entire notional of the trade to spread capture
with an equal and opposite error in drift — while `gross()` and `net()` stay correct,
so nothing looks wrong. Such fills go to `unattributed_shares_` / `unattributed_cash_`,
printed beside the attribution. `Mid::two_mid` is only readable through an accessor
that refuses when the status is unusable, so the poisoning cannot be written.

**At the horizon endpoint.** Dropping here **conditions the sample on the future**.
Books go one-sided when they are being swept, which is when adverse selection is
largest; halts follow the fills that immediately preceded them. Missingness is
correlated with the outcome, so the surviving mean is biased toward zero drift — i.e.
optimistic. So: never drop silently. Report the endpoint-drop rate per side and per
cohort, and publish a **bracketing bound** — the mean recomputed with dropped samples
carried forward at the last usable mid, and again at the surviving touch — so the
reader sees the interval the exclusion spans. On MSFT intraday these counts will be
near zero; say so, and let the bias matter only where it is real.

**Truncation is decided at push time, not drain time.** If `ts + h > continuous_session_end_ns`,
record immediately as `PastSessionEnd`, never queue it, exclude from the horizon mean,
and print its own count and its own mean. Dropping truncated samples silently deletes
precisely the last-10-seconds-of-session fills, which are systematically different.
`FeedEnded` is a **separate** status for "the input file stopped first" (a short slice,
`--limit`) — conflating "the session ended" with "your input stopped" hides a truncated
slice.

`continuous_session_end_ns` is **not** the replay window bound. `validation/MSFT_2019-12-30.json`
records `session_end_ns = 68400000000000` = 19:00 ET — the phase-2 UTC-day boundary,
not the 16:00 close (57600000000000). Reusing that name or that value means
`PastSessionEnd` never fires near the close and every 10 s markout for a 15:59:5x fill
resolves against the thin after-hours book. Both values are printed in the report
header.

### 5.6 Aggregation

Headline: **share-weighted mean**, `Σ(q_i · x_i) / Σ q_i`, numerator kept as an exact
`int64`, reported in **cents per share** — the unit fees are quoted in, so 0.30 c/share
of adverse selection nets directly against a $0.0030 rebate. Only dispersion touches
floating point (Welford; a sum of squares over `q · two_drift` overflows `int64` at
~1e20).

Alongside it, always: the unweighted per-fill mean (the gap says whether large fills
are systematically more toxic, which is what you expect — you get filled in size
exactly when someone is sweeping), p10/p50/p90 from the raw sample vector sorted at
report time (markouts are a spike at zero with mass at ±half a tick; a mean alone is
misleading), and the sample count.

Basis points use the **mid** as the denominator, not the fill price:
`bps = per_share_micros * 200 / two_mid`. A fill-price denominator is smaller for buys
than for sells by the spread and manufactures a fake side asymmetry. On a $157 stock
the whole phenomenon lives between 0.1 and 0.5 bps, which is why bps is the wrong
headline and earns its place only for cross-symbol comparison.

**Confidence intervals from non-overlapping blocks, not from `n_eff`.**
`(Σw)²/Σw²` corrects only for unequal *share* weights. It does not touch the
dependence that dominates: **overlapping horizons**. Two fills one second apart at a
10 s horizon share 90% of the same mid path. MSFT 2019-12-30 has 40,321 printable
trades over a 23,400 s session (~1.7/s), so a 10 s window contains ~17 overlapping
samples and the number of effectively independent 10 s blocks is ~2,340, not ~38,000.
With a 10 s mid standard deviation of ~3.9 c/share, the true standard error is
~3.91/√2340 ≈ 0.081 c/share, giving a 95% CI of about ±0.16 c/share — **wider than the
entire $0.0020 base rebate**, and about 4× wider than the naive figure. Publishing a
too-narrow standard error as the guard against overconfident claims is worse than
publishing none, because it licenses exactly the claim it was meant to prevent.

So: report the standard error from non-overlapping blocks of length ≥ the horizon, or a
moving-block bootstrap with block length 2–3× the horizon. Report **both** the naive
`n_eff` and the block-based effective `n` so the gap is visible. Consequence to state
plainly: the 100 ms markout is measurable on a single day; the 10 s markout is not.

Breakdowns: cohort × horizon × side × liquidity, plus an odd-lot/round-lot split at
100 shares (odd-lot executions have systematically different toxicity, and pre-2024
Rule 605 excludes them entirely).

### 5.7 The market-wide passive benchmark

Every `E` and printable `C` in the feed is a passive fill for *somebody*, and ITCH
gives the resting order's side directly. So `book.find(ref)` before `book::apply()` —
one extra probe on the ~2.9% of messages that are executions, and free if fused into
`apply_ex()` — yields the **market-wide passive markout** as a fourth cohort at
essentially zero cost.

Our strategy's markout is then reported next to the market's. A strategy whose fills
are much *better* than the population's is evidence the queue model is too optimistic,
not evidence of alpha. Trade signing is exact here — ITCH tells you the resting side,
so there is no Lee-Ready classification error to explain away, which is a real
advantage over the academic literature.

`MarketPassive` is routed to the **markout engine only, never to a `Ledger`**. A P&L
ledger for the entire market's passive side has a position that is the negative of
every aggressor's net position for the day, a meaningless cash balance, and a
break-even maker rate that is nonsense which would nonetheless print into the report.

**Rule 605, demoted honestly.** SEC Rule 605 publishes average effective and realized
spread per market centre per symbol per month. With signs corrected:

```
effective_spread = +2 * edge          (NOT -2 * edge)
realized_spread  =  2 * markout(300s) = 2 * (edge + drift)   (NOT 2 * drift)
```

This is an **order-of-magnitude and sign check, not a few-percent agreement**, and it
is strictly weaker than the phase-2 Databento bar, which really was exact. Reasons:
605 uses the **consolidated BBO** midpoint and NASDAQ is frequently not at the NBBO;
605 uses the mid at **order receipt**, not at execution; the covered-order population
(market and marketable limit, 100–9,999 shares, regular hours, no odd lots pre-2024,
no special handling, no auction) is a small non-random subset of every `E` on the tape;
and it is a monthly aggregate by size bucket being compared against one day. To make it
bite harder, restrict the `MarketPassive` cohort to 100–9,999-share executions in
regular hours and report per size bucket.

### 5.8 Fees: one signed field, in micro-dollars

```cpp
using Money = int64_t;                  // 1e-6 USD
constexpr Money kMicrosPerPrice4 = 100; // Price(4) is 1e-4 USD
```

**Positive = you pay. Negative = you receive.** The ledger then does `cash -= fee`
unconditionally with no is-this-a-rebate branch, and an inverted venue (pay to add,
get paid to take) is a config file rather than a code path. Micro-dollars represent
every published rate exactly: $0.00300 = 3000, $0.00305 = 3050, $0.000119 = 119.

There is **no "mils" unit anywhere.** A per-share unit of tenths of a cent invites a
10× error — `maker = -20` reads as a $0.0020 rebate and is a $0.020 rebate, four times
the entire gross edge of a one-cent spread, which turns the headline chart into a
chart of the rebate. Micro-dollars are unambiguous and every rate is an exact integer.

Three **separate** accumulators, never mixed:

| bucket | contents |
|---|---|
| `exchange_fees_` | maker / taker / cross rate |
| `regulatory_fees_` | SEC Section 31 + FINRA TAF, **sells only** |
| `unattributed_` | fills with no usable mid (see §5.5) |

Defaults (NASDAQ, ≥ $1.00, per share, circa 2019):

* taker (remove) `+3000` ($0.0030)
* maker (add) `-2000` base tier ($0.0020 credit); `nasdaq_top_tier_2019()` at
  `-3050…-3100`
* opening/closing cross `+1500` ($0.0015)
* inverted venue: maker `+1800`, taker `-1600`

Sub-$1.00 names switch to a notional rule (NASDAQ prices those as a percentage of
dollar value); per-share rates are simply wrong there, so the schedule is a rule, not
a scalar.

**Regulatory costs are not a rounding detail on a $157 stock, and omitting them
changes the sign of the headline.** They are notional-based, so the per-share cost
scales with price:

| charge | rate | on MSFT at $157.57 |
|---|---|---|
| SEC Section 31 (sells only) | $20.70 per $1,000,000 of proceeds | **0.326 c/share** |
| FINRA TAF (sells only) | $0.000119/share, capped $5.95/trade | 0.012 c/share |
| **total, sell leg** | | **0.338 c/share** |
| **averaged over a two-leg round trip** | | **0.169 c/share** |

That is comparable to the entire $0.0020 base maker rebate and to the whole
adverse-selection effect being measured — and it is ~8× the figure you get by
reasoning about a ~$20 stock. `FeeSchedule` therefore has a per-notional component;
a per-share-only struct is structurally incapable of expressing Section 31.

**The rate is date-dependent and must be loaded from a dated config with its source
cited, not baked into a header.** The Section 31 rate is reset by the SEC (the
$22.10/M rate took effect in 2020; $20.70/M is the value believed in force on
2019-12-30, and it must be verified against the SEC fee advisory before publication).
Every report prints the schedule name and its effective date so a number generated in
2027 is still interpretable. `--maker-micros` may be **required with no default** so
the number can never be reported without someone having chosen a tier.

The TAF cap is **per trade**, so a sweep that fills as N partials is capped N times in
this model — a small overstatement, flagged rather than silently applied.

### 5.9 Fee/model interaction

1. `fee_for(schedule, fill)` is a pure function of the `FillEvent` and the schedule,
   **identical for all four models**. The models differ only in which fills exist.
2. The queue model and the strategy are **fee-blind**. A fee-aware strategy would trade
   differently under each model and the comparison would stop isolating the queue
   assumption.
3. All models are marked at the same terminal price, and residual position is reported
   per model. A comparison in which pessimistic ends flat and naive ends long 300
   shares is not a comparison.
4. Fees accumulate in micros and round once at the end, per model. Per-fill rounding
   gives a model with 3× the fills 3× the rounding error.
5. Every headline figure is **per share**. Totals scale with fill count, so a model
   that fills 3× as much trivially has 3× the spread capture and 3× the fees; the fill
   count is reported beside as its own result.

### 5.10 Break-even maker rate

```
break_even_maker_rate = (gross - taker_fees - cross_fees - regulatory_fees) / maker_shares
```

**Positive means the strategy can afford to pay that much to add**, under the
schedule's sign convention (positive = you pay). Getting this comment backwards is the
easiest error in the report, and it prints in the headline table.

`regulatory_fees_` must be subtracted explicitly: it is charged on maker sells too, so
lumping it into a "non-maker fees" bucket silently solves ~0.17 c/share into the maker
rate — larger than the gap between the base and top rebate tiers, which is the whole
point of the table. Round explicitly; integer division truncates toward zero and
rounds a negative numerator the wrong way.

---

## 6. Strategy interface and P&L accounting

### 6.1 Dispatch

A duck-typed template parameter checked by a C++20 `concept`, exactly as
`itchbook::parse()` takes a Handler — the dispatch inlines, there is no indirect call
on a path that runs ~1.2M times per symbol-day, and a missing callback produces one
readable diagnostic instead of a template instantiation dump.

```cpp
template <class S>
concept Strategy = requires(S s, const Event& e, const MarketView& m, Ctx& c) {
    { s.on_event(e, m, c) } -> std::same_as<void>;
};
```

Optional callbacks are detected with `if constexpr (requires { ... })`:
`on_start`, `on_fill`, `on_ack`, `on_reject`, `on_cancelled`, `on_halt`, `on_timer`,
`on_end`. No virtuals, no `std::function`, no allocation per event.

### 6.2 What the strategy sees, and how look-ahead is made structurally impossible

`on_event(const Event&, const MarketView&, Ctx&)` fires **once per exchange event**
(timestamp group, §4.4), on the strategy's delayed clock.

Guarantees, all structural rather than by convention:

1. `MarketView` exposes only read accessors bound to the **delayed view book**. The
   simulator never hands out the exchange book or the `Matcher`. The simulator itself
   may read exchange state for instrumentation (`filled_at_send`); the strategy may
   not, ever. If the strategy can reach the exchange book, every number is silently
   wrong and no test trips.
2. `Ctx` methods **queue intents**; nothing executes inline. An intent raised on event
   `k` cannot arrive before the end of the timestamp group of event `k`, and with the
   latency floor it cannot arrive within it at all.
3. Every `Event` field is derivable from messages `0..k`. The two not on the wire —
   the resting price for `E`/`X`/`D` and the maker side — are resolved from the
   pre-mutation book, which is reconstructed state, not the future.
4. `Ctx::now()` is the strategy's clock (`t_exch + feed`) and there is no other clock.
5. Fills arrive only through `on_fill`, in timestamp order.
6. **Markouts are computed offline from the fill tape after the replay finishes.** They
   are future information by definition and are structurally unreachable from `Ctx` and
   `MarketView`. Saying so explicitly is worth more than the code that enforces it.

`MarketView` splits two things the design must not conflate. Our orders are not in the
feed book, so `best_bid()`, `shares_at()` and `top()` **exclude our own resting
quotes** — a quoter that conditions on "am I at the touch" would otherwise churn
against its own invisible order. So `MarketView` offers both: the feed-book queries,
and separately `our_shares_at(side, price)`, `our_working()` and
`effective_best_bid()/effective_best_ask()` (feed plus our own). Which one a strategy
reads is an explicit choice.

`Event` carries `top_before`, `top_after` and **two** change flags. `Quote` carries
`bid`, `ask`, `bid_size`, `ask_size` (all available for free from `book::LevelView`)
and a defaulted `operator==`, so `top_size_changed` is a capability the type actually
has — a `Quote` with prices only can never detect the touch draining from 5,000 shares
to 100 at the same price, and a comment claiming otherwise is worse than no comment.

`Event` also splits two concepts that a single `is_trade` flag conflates:

* `is_execution` — `E`, printable `C`, **and non-printable `C`** — the
  queue-consumption signal.
* `is_printable_trade` — `E`, printable `C`, `P`, `Q` — the volume/VWAP/markout signal.

Keying queue advancement on `is_trade` as "E, printable C, P, Q" would silently fail
to advance on non-printable `C` (576 messages on the validated day — small, but a
systematic sign error in the under-filling direction) *and* would advance on `P` and
`Q`, which never touch the book at all.

`Event::has_side` is **false for both `P` and `Q`**. `P`'s Buy/Sell indicator is the
side of the non-displayed order and is not usable information (§2.7).

### 6.3 Queue position is not exposed by default

`Ctx` has no `queue_ahead()`. A strategy may opt in with
`static constexpr bool kUsesQueuePosition = true`, which enables
`Ctx::queue_ahead(OrderId) -> std::pair<uint64_t,uint64_t>` (the lane's own bracket)
and makes the harness **refuse frozen-decision mode**, printing why.

The justification is *not* that hiding it keeps the controlled experiment possible —
it does not, because `Ctx::position()`, `Ctx::realised()`, `working()` and `on_fill`
are all fill-model-derived, and any strategy that reacts to its own fills already makes
Mode A three trajectories. The real justification is narrower and true: **queue
position is an estimate whose error bars are the subject of this phase, so consuming it
silently launders modelling uncertainty into a trading decision.** Returning the
bracket rather than a point estimate is the same principle.

### 6.4 Risk limits belong to the harness, not to strategy code

A position limit is by definition a function of fills, so a strategy that enforces its
own cap cannot be fill-independent, and a `strategy_fill_independent` bool that the
strategy author sets is a comment with a type — unfalsifiable in a single-pass design
where there is only one order stream by construction.

So:

* Headline strategies satisfy a stricter concept, `HeadlineStrategy`, requiring
  `decide(const MarketView&, uint64_t now) -> Action` with **no `Ctx`, no fill
  callbacks, no position accessor**. A fill-dependent strategy cannot compile as a
  headline strategy. That is the enforcement.
* The position limit is a **harness-enforced pre-trade check** applied identically to
  live and replayed intents: a post that would take `|pos|` past the cap is rejected
  with `Reject::RiskLimit` before it reaches the fill model. It becomes a property of
  the experiment rather than of strategy code, it keeps the tape replayable, and it
  bounds every lane's inventory.
* Mode B additionally **hashes the intent tape per lane and asserts the four hashes are
  equal** (**I13**), and reports the per-lane count of tape entries rejected on replay.
  If the naive lane rejects 30% of the tape, Mode B is not the clean control it claims
  to be either, and the reader must be able to see that.

### 6.5 Money and the ledger

`Money = int64_t` micro-dollars. **No `double` anywhere in the accounting path**;
floating point appears only in report formatting and in the Python chart. NASDAQ fees
have five decimal places, so cents are too coarse and `Price(4)` is too coarse. A $100M
notional day is 1e14 against an `int64` range of 9.2e18. The deeper reason: the
headline claim is that four numbers differ by a *small* amount, and floating-point
summation error over ~1e5 fills is small but not reproducible across compilers,
optimisation levels or `-march=native`. Exact integers let the tests assert equality
rather than an epsilon.

**Authoritative number:** `equity(mark) = cash + position * mark`, with all fees folded
into `cash` at fill time.

**Attribution is three terms, not two:**

```
equity(mark) == edge + drift(mark) + fees
edge         =  Σ q_i·sgn_i·(two_mid_i − 2F_i) · 50
drift(mark)  =  Σ q_i·sgn_i·(two_mark − two_mid_i) · 50
```

Defining `holding = equity − edge` dumps the entire fee and rebate stream into the
bucket labelled adverse selection. On MSFT at a one-cent tick the rebate is 20% of the
gross spread, so the report would show a market maker whose "adverse selection" is
systematically flattered by its own rebates — and printing gross and net side by side
would be impossible.

**Realised/unrealised** is a *presentation* of that number, not its source: FIFO, LIFO
and weighted-average cost differ only in how a total is split and never in the total.
WAC, with the basis computed as
`(open_cost/open_abs)*closing + (open_cost%open_abs)*closing/open_abs` — no 128-bit
intermediate, because `__int128` trips `-Wpedantic -Werror` on this project's build.
C++ truncation toward zero keeps the identity for negative `open_cost` too. A full
close is exact, so `pos == 0 ⟹ open_cost == 0 ⟹ realised == cash`.

**`cash + open_cost == realised` is a typo guard, not a conservation law.** Expand the
deltas: since `notional()` is linear in quantity, `trade_cost == closing_cost + open_notional`
identically, so the identity holds for *any* value of `basis`, *any* value of `fee`,
and any split of `|q|` into closing + opening. It cannot detect a wrong WAC basis, a
sign error on the maker rebate, a maker/taker misclassification, a wrong `two_mid`, a
wrong position update (position does not appear in it at all), or a fill attributed to
the wrong side. Do not bill it as this phase's `conserves_shares()`. The checks with
teeth are **I8** and **I9** in §9.

### 6.6 The taker path

`sim/taker.hpp`. Walks `Book::top(opposite, n, &levels)` and computes a fill schedule
**without mutating the feed book**:

```cpp
struct TakeFill { int32_t price; uint32_t shares; };
struct TakeResult {
    uint32_t filled = 0, unfilled = 0;
    int32_t worst_price = 0;
    uint32_t levels_walked = 0;
    bool depth_capped = false;             // hit max_pct_of_level
    std::vector<TakeFill> fills;
};
TakeResult take(const book::Book&, Side, uint32_t qty, int32_t limit,
                uint32_t max_pct_of_level, int32_t slippage_ticks_beyond_first);
```

The modelling assumption is stated, not hidden: **our takers consume liquidity that
the feed also consumes; displayed depth is not decremented.** Against a single
unmutated book, the same 100 resting shares would otherwise be sold to all four lanes
*and* to whichever real aggressor in the feed actually consumed them — four-way
double-spending of one order, worst exactly at a forced end-of-day flatten where every
lane dumps its whole inventory at once, and largest for the lane with the most
inventory.

Bounds and reporting: cap participation at `max_pct_of_level` of displayed size per
level, charge configurable slippage in ticks beyond the first level, and report
`taker_shares` as a fraction of the day's displayed volume per lane so a reader can
size how much of the P&L rests on it. **I14** asserts `Book::unknown_ref()` and
`resting_shares()` are byte-identical to a control run with no strategy.

`engine::Matcher` is not used here, for the reasons in §1.2 and §11 trap 7.

### 6.7 Session boundaries and the terminal mark

There is **no session start gate anywhere in a naive design**, and the consequence is
severe: the ITCH file carries orders from 04:00 ET and `book::modelled()` accepts
`A`/`F`/`D`/`U` from the first message of the day, so a quoter runs through the entire
pre-market and, worst, through the 09:28–09:30 opening-book accumulation window where
the NASDAQ book is **legitimately crossed by construction** (orders accumulate without
matching until the opening cross). A cancel-on-crossed rule then churns on essentially
every event in that window.

So, all harness-owned, none in strategy code:

| boundary | default | action |
|---|---|---|
| session open | `S = 'Q'` | intents released |
| quote stop | 15:45:00 ET | cancel every working order (`CancelReason::EndOfDay`); further posts rejected with `Reject::SessionClosed` |
| flatten window | 15:45–15:55 ET | IOC through the scratch overlay, retried each event |
| session close | `S = 'M'` | no intents; drain and resolve everything in flight |

`Comparison::on_message` copies `book_replay.cpp`'s locate filter **including the
`type != 'S'` exemption** — System Event messages carry no meaningful locate but bracket
the session, and without the exemption the sim can never find 09:30 or 16:00.

**The terminal mark is not the closing cross.** The headline P&L is 100% realised with
`position == 0` asserted (which the harness quote-stop makes an enforceable invariant
rather than a hope). Any residual that could not be flattened — halt, thin book — is
marked at the **last usable continuous-session mid at or before the flatten deadline**,
reported on its own line with the share count and the mark source named, and the
headline P&L line annotated as containing an unrealised residual.

The closing cross price is reported **separately as post-flatten context, never as the
mark**, for three reasons: you cannot transact there from 15:58 (the MOC/LOC entry
deadline was 15:50 ET in 2019, and after it you may only enter on the offsetting side
of the published imbalance); the cross prints at 16:00, so marking a 15:5x residual
with it is forward-looking information inside a reported number; and it contradicts
the design's own — correct — reason for excluding our orders from crosses, which is
that we were not in the auction. You cannot both be excluded from the auction and be
marked at its price. `Book::cross_prices()` returns a **const** map, so it is read with
`.count('C')` then `.at('C')`, never `operator[]` (which does not compile on a const
map) and never unguarded `.at()` (which throws on a short slice).

Force-flattening makes the number **transactable**, which is a real improvement over a
mid mark. It does not make it a measure of fill quality: the headline still contains
Δinventory × (flatten price − average entry) plus a Δ(taker fee) term. So the report
also carries **risk-normalised figures** — P&L per filled share, and P&L per unit of
average `|inventory|` — which is what actually makes the four bars comparable.

---

## 7. The trivial strategy, and why it should lose money

Two different jobs, and conflating them is the mistake:

* **The headline strategy** must make the *fill model* the variable. It is chosen
  because its sign plausibly flips between models — not because it is guaranteed to
  lose.
* **The loss guarantee** the build plan demands ("run a strategy you *know* is
  unprofitable and confirm it loses money") needs a strategy whose expected result is
  known **before the code runs**. The headline strategy cannot do that job, because on
  a quiet day with rebates its sign is genuinely uncertain, and a test whose expected
  outcome you are unsure of is not a test.

### 7.1 `TouchMaker` — the headline

Every parameter fixed a priori, none tuned:

| parameter | value | why |
|---|---|---|
| quote size | 100 shares | one round lot, from `'R'` `round_lot_size`. Small enough that zero market impact survives against a 1,000–5,000-share touch; large enough that the fill model can tell the four worlds apart (a 1-share order fills on any trade). |
| placement | **join only** — bid at `best_bid`, offer at `best_ask` | An order that *improves* the touch creates a level with `ahead == 0` in all four models, and the four bars collapse into one. |
| sides | one live order per side, asserted | Two of our orders at one price makes naive double-fill against a single print and breaks **I7**. |
| requote | on `top_price_changed`, cancel and repost if not at the current best | Never reprice in place: a `U` goes to the back of the level and the sim charges that loss of priority. One requote per side per event. |
| size reaction | **none** | Reacting to depletion at our own price is queue-aware behaviour, gated by `kUsesQueuePosition`. Deliberate omission. |
| skew / hysteresis / min rest | none | A skew is a fair-value model and this strategy has none. Every parameter added is a parameter a reader can accuse you of tuning until the chart said what you wanted. |
| inventory | **blind** for the headline (Mode B); harness cap ±500 in Mode A | §6.4 |
| session | harness-owned (§6.7) | |
| crossed / locked / one-sided book | quote nothing | |

`--requote-ns` is deliberately **not** a default 1 ms interval. A quote that cancels
and rejoins every millisecond never accrues queue position: naive fills constantly,
pessimistic and `mbo` barely fill at all, and `mbo` collapses onto pessimistic for a
reason that has nothing to do with the market. The requote policy is a **first-class
parameter of the finding** (join-and-hold vs requote-on-touch-move vs fixed interval)
and the band is published at more than one setting.

The headline run must report `max |position|` and the inventory-carry term separately.
An inventory-blind quoter's position is a random walk in fill imbalance; because the
report decomposes into edge + drift + fees and normalises per share, that term is
visible and attributable rather than hidden in the total.

### 7.2 Why it should lose on real data

1. **Queue length.** MSFT at $157 rests 1,000–5,000 shares at the touch and you join
   behind all of it with 100.
2. **The cancel-to-execution ratio.** On the validated day, `D` 548,858 + `X` 840 +
   `U`-delete 54,873 = **604,571** ambiguous removals against `E` 34,739 + `C` 576 =
   **35,315** executions — **17.1 cancels per execution**. Under pessimistic queueing,
   only executions advance you, so a level that turns over by cancellation never
   advances you at all.
   *Caveat the write-up must carry:* that is a **whole-book message count**, and queue
   advancement is in **shares at the touch while your order rests there**. `D` carries
   no share count on the wire, so the share-weighted figure requires resolving each `D`
   against `book.find(ref)->shares` — which `resolve()` already does. **The sim
   computes and publishes the share-weighted cancelled-vs-executed ratio at our own
   level while we rest there**, and the 17.1 appears only as context with both
   qualifiers attached. Quoting the whole-book message ratio as the headline queue
   statistic is the kind of thing a practitioner will challenge in the first minute.
3. **Adverse selection.** The fills you get on the bid are the ones where sellers kept
   coming, so the mid then falls. A symmetric quote with no fair-value model has
   nothing to offset it.
4. **Latency.** You cancel N microseconds late, and the fills in that window are by
   construction the ones where the price already moved through you, so the loss is
   monotone in N.
5. **The tick binds.** MSFT's modal spread is one cent, so gross round-trip capture is
   $0.01/share against a $0.0030 taker fee to flatten.
6. **Regulatory costs on the sell leg** (§5.8) are ~0.169 c/share averaged over a round
   trip, against a base maker rebate of 0.20 c/share. The rebate very nearly cancels.

### 7.3 A worked headline — **illustrative numbers, not measured**

MSFT scale: ~$157.60, one-cent spread, half-spread = 0.50 c/share.

| | naive | optimistic | mbo | pessimistic |
|---|---|---|---|---|
| edge | +0.50 | +0.50 | +0.50 | +0.50 |
| 1 s drift (assumed) | −0.21 | −0.38 | −0.49 | −0.62 |
| maker rebate (base tier) | +0.20 | +0.20 | +0.20 | +0.20 |
| regulatory, round-trip avg | −0.169 | −0.169 | −0.169 | −0.169 |
| **net, c/share** | **+0.32** | **+0.15** | **+0.04** | **−0.09** |

Break-even maker rate under pessimistic: `0.62 + 0.169 − 0.50 = 0.289` c/share ≈
**$0.0029/share** — the *top* tier, not comfortably below the base tier. Strip the
rebate entirely and every model loses. Put it on an inverted venue charging $0.0018 to
add and all four go negative.

Exiting aggressively costs `0.50` (half-spread) `+ 0.30` (take fee) `+ 0.338`
(regulatory, if the exit is a sell) = **1.14 c/share**, so a passive-in/aggressive-out
round trip needs about a tick and a half of edge, not half a tick.

The point of the table is that "the queue assumption changes the P&L" and "the fee
schedule changes the sign" are the same finding viewed twice — and that omitting the
regulatory row moves the pessimistic bar from −0.09 to +0.08, i.e. **flips the
headline conclusion**.

### 7.4 The oracles — strategies with known answers

Each is a ctest case using `tests/check.hpp`.

**S1 `Crosser` — the loss guarantee.** Alternate a 100-share IOC buy and sell every N
`top_price_changed` events. Two assertions:

* **Exact accounting**, from the fill audit record: the test re-derives realised P&L
  from `(event_index, side, fill price, qty, fee, liquidity)` rows and asserts equality
  with the incrementally maintained ledger, to the micro-dollar. This is what S1 can
  honestly assert exactly.
* **The loss**, a priori: the round trip is `−(ask_at_buy − bid_at_sell)`, i.e. the
  spread **plus the price drift between the legs** — MSFT ran 159.20 → 156.73 that day,
  and a strategy systematically long between its two legs accumulates that. And even
  the drift-inclusive figure cannot be asserted exactly against the *decision-time*
  touch, because the IOC does not arrive until later and `Crosser` fires on a touch
  move. So assert the statement that is genuinely a priori: every completed round trip
  has non-positive spread-and-fee contribution, and over ~1,200 round trips the sum of
  (spread + 2 × taker fee + regulatory) ≈ $1,900 dominates the drift term (σ ≈ $70) —
  assert `realised < −$1,000`. On a synthetic flat-mid feed with zero drift, assert the
  exact figure.
* Byte-identical under all four fill models: a taker fill has no queue.

**S2 `FarQuoter`.** Bid at $0.01 and offer at $199,999.99 — the stub-quote prices real
feeds carry. Zero fills, zero fees, zero P&L in every model, with orders working all
day. **Not** `Price(4) = 1` ($0.0001): that is a sub-penny price, illegal under Rule
612 for a $157 stock, and `Ctx::post` rejects it with `Reject::InvalidPrice`, so the
test would silently have one working order instead of two. Note honestly what S2 does
and does not exercise: both prices are alone on empty levels, so `ahead == 0` in all
four models and this is the *degenerate* path (which is also why it belongs as a
separate oracle — see S7). Both are outside the dense band, so it also exercises the
overflow map.

**S2b — the queue machinery, separately.** Post at the touch behind a known amount of
resting size on a hand-built feed and assert `ahead` evolves exactly as computed by
hand under each model. This is the test that can actually fail.

**S3 `Flicker`.** Post, cancel on the next event, forever. Assert the invariant that is
true: every fill timestamp lies in `[ack_ns, cancel_ack_ns]`, and the exposure window
is exactly one exchange event at zero latency. Do **not** assert the fill count is
zero — the order is live for one full fill-model pass and will fill if that event is an
execution at its price with `ahead == 0`. Instead assert the count is **non-zero** on a
feed with trades at the quoted price: a `Flicker` that never fills is evidence the
exposure window is being skipped, which is the bug this test exists to catch.

**S7 — inside-the-spread equality.** A strategy that only ever posts at a price where
no level exists must produce **identical P&L in all four models** (`ahead == 0`
everywhere). Together with S1's cross-model equality, this asserts that the fill model
is confined to exactly the code path where queue ambiguity lives and has not leaked
into the taker path or the empty-level path.

---

## 8. Files to create, with signatures

Everything is header-only. **Every free function is `inline`** — the CMake target is
`add_library(itchbook INTERFACE)` with no `src/`, so a non-inline definition in a
header is a multiple-definition link error the moment two translation units include it.

```
include/itchbook/sim/
  money.hpp          Money, notional(), Liquidity
  fees.hpp           FeeSchedule, fee_for()
  mid.hpp            Mid, MidStatus, observe()
  event.hpp          Resolved, Trigger, SimFill, at_or_through()
  queue_model.hpp    Model, QueueModel, Entry, AheadSet, ModelBank   [build plan]
  latency_model.hpp  Channel, LatencyModel, EventKey, Action, Outcome [build plan]
  taker.hpp          take() against a scratch overlay
  strategy.hpp       Strategy / HeadlineStrategy concepts, Event, Quote, MarketView, Ctx
  markout.hpp        MarkoutEngine, Sample, HorizonReport
  ledger.hpp         Ledger
  report.hpp         RunReport, write_json / write_fills_csv / write_equity_csv
  backtest.hpp       Comparison<Strategy> — the parse() handler, four lanes, one pass
  strategies.hpp     TouchMaker, Crosser, FarQuoter, Flicker, Recorder<S>, TapePlayer

tools/queue_backtest.cpp      the driver
tools/latency_sweep.cpp       the grid

python/
  make_queue_feed.py          book-legal feed with real queue structure
  make_toxic_feed.py          informed flow with a known answer + ground-truth queue
  reference/queue_sim.py      the slow, obvious oracle
  analysis/fill_comparison.py headline chart (stdlib SVG + CSV + --ascii)
  analysis/latency_sweep.py   P&L vs N
  analysis/markout.py         markout tables

tests/
  test_queue.cpp              hand-computed micro-scenarios
  test_markout.cpp
  test_fees.cpp               goldens at MSFT's real price
  test_sim.cpp                S1, S2, S2b, S3, S7
  fuzz/fuzz_queue.cpp         the invariants of §9
  queue_differential.py       C++ vs python/reference/queue_sim.py, byte-identical
docs/phase6-results.md
validation/queue_MSFT_2019-12-30.json
```

### 8.1 Prerequisites in existing headers

These must land **before** any of the above, and each is a small, mechanical change:

```cpp
// book/book.hpp — const overloads, non-const today purely by omission
uint64_t     Book::shares_at(char side, int32_t price) const;
const Order* Book::best_order(char side) const;
const Level* Side::level_at(int32_t price) const;
const Level* Side::level_of(const Order*) const;

// book/book.hpp — NEW. The ahead-set cannot be built without it: there is
// currently no way to get from an arbitrary (side, price) to that level's head.
// best_order() gives only the best level's head.
const Order* Book::first_order(char side, int32_t price) const;

// book/book.hpp — NEW. The sim needs a locked/crossed distinction; Book::crossed()
// is `b >= a` and counts a locked book as crossed. Leave crossed() alone (the
// matcher's fuzz invariant depends on it) and add:
bool Book::locked() const;             // b == a
bool Book::strictly_crossed() const;   // b >  a

// book/dispatch.hpp — NEW. One ref-map probe instead of three.
struct PreState {
    bool known = false;
    char side = 0; int32_t price = 0; uint32_t shares = 0; uint64_t ref = 0;
};
inline bool apply_ex(Book&, char type, const uint8_t* p, PreState* out);
```

Without `apply_ex`, `resolve()` adds a **third** independent walk of the ref map on
the 52% of messages that are `E`/`C`/`X`/`D`/`U`, on the hottest path in the program —
undoing part of phase 4 in a design that justifies template dispatch on the grounds
that an indirect call per event is unaffordable. It also removes the possibility of
`resolve()` and `apply()` disagreeing about which order a reference names, which is a
real hazard given `RefMap::insert`'s documented last-writer-wins on duplicates.

### 8.2 Known Phase 5 gaps (recorded, not necessarily fixed here)

* `Matcher::match()` breaks out on an external maker (`matcher.hpp:302-304`), so it
  cannot trade against feed liquidity. Phase 6 routes around it (§6.6). Fixing it
  properly requires an external-maker path plus reworking `conserves_shares()`
  (`filled_total_ += 2*qty` assumes both sides are ours), STP (`maker.req.owner == 0`
  would make every external maker look like a self-trade), and
  `Reject::NoLiquidity`/`FokUnfillable` (which currently *measure* external depth via
  `available()` but cannot consume it).
* `Matcher::last_trade_` is written in exactly one place — inside `match()` — so stops
  never trigger from market activity. Phase 6 triggers stops from the **feed's prints
  on the exchange clock**, from the exchange book, never from the delayed view.
* `engine::transition()` uses `assert(legal_transition(...))`, a no-op under `NDEBUG` —
  which is the Release build the published numbers come from. Phase 6 gates its own
  checks on `-DITCHBOOK_SIM_CHECKS`, on for publishable runs.
* `RefMap::insert` resolves a duplicate reference by last-writer-wins, orphaning the
  previous `Order` inside its `Level`. Not reachable in Phase 6 given §1.2, but it is
  reachable the moment anyone injects orders into a feed-populated book.

### 8.3 Core signatures

```cpp
// ---- sim/money.hpp ---------------------------------------------------------
namespace itchbook::sim {

using Money = int64_t;                              // 1e-6 USD
inline constexpr Money kMicrosPerPrice4 = 100;      // Price(4) is 1e-4 USD
inline constexpr Money kMicrosPerCent   = 10000;

inline constexpr Money notional(int32_t price4, int64_t qty) {
    return static_cast<Money>(price4) * qty * kMicrosPerPrice4;   // signed with qty
}

enum class Liquidity : uint8_t { Unset = 0, Maker, Taker };

}  // namespace itchbook::sim

// ---- sim/fees.hpp ----------------------------------------------------------
// Positive = you pay, negative = you receive. One signed field per rate, so the
// ledger does `cash -= fee` with no branch and an inverted venue is a config file.
struct FeeSchedule {
    const char* name      = "nasdaq-base-2019";
    const char* effective = "2019-12-30";          // printed in every report

    Money maker =  -2000;   // $0.0020 credit, base tier
    Money taker =   3000;   // $0.0030
    Money cross =   1500;   // $0.0015 (unused while CrossPolicy::Exclude)

    int32_t sub_dollar_price       = 10000;        // $1.00 in Price(4)
    int64_t sub_dollar_taker_nanos = 3000000;      // 0.30% of dollar value
    int64_t sub_dollar_maker_nanos = -500000;      // 0.05%

    // SELLS ONLY. Notional-based, so per-share cost scales with price: 0.326 c/share
    // on MSFT at $157.57, ~8x what the same rate costs on a $20 stock.
    int64_t sec31_nanos_of_notional = 20700;       // $20.70 / $1,000,000 (FY2019)
    Money   taf_per_share           = 119;         // $0.000119
    Money   taf_cap_per_trade       = 5950000;     // $5.95

    static FeeSchedule nasdaq_base_2019();
    static FeeSchedule nasdaq_top_tier_2019();     // maker -3050..-3100
    static FeeSchedule inverted_2019();            // maker +1800, taker -1600
    static FeeSchedule zero();
    static bool load(const char* path, FeeSchedule* out);   // "key value" lines
};

struct FeeBreakdown { Money exchange = 0; Money regulatory = 0; };
inline FeeBreakdown fee_for(const FeeSchedule&, const SimFill&);

// ---- sim/mid.hpp -----------------------------------------------------------
enum class MidStatus : uint8_t {
    Empty = 0, Ok, Locked, OneSided, StrictlyCrossed, Halted, OutsideContinuous,
};
inline bool usable(MidStatus s) { return s == MidStatus::Ok || s == MidStatus::Locked; }

// two_mid is only readable through an accessor that refuses on an unusable status,
// so a 0 sentinel cannot reach the ledger and mis-attribute a whole trade's notional
// to spread capture (see 5.5).
class Mid {
public:
    static Mid observe(const book::Book&);          // const-only; gates on phase first
    MidStatus status() const { return status_; }
    bool ok() const { return usable(status_); }
    int64_t two_mid() const {                       // best_bid + best_ask, Price(4)
        ITCHBOOK_SIM_CHECK(ok());                   // never `assert` — see trap 27
        return two_mid_;
    }
    int32_t bid() const; int32_t ask() const;
private:
    int64_t two_mid_ = 0; int32_t bid_ = 0, ask_ = 0;
    MidStatus status_ = MidStatus::Empty;
};

// ---- sim/event.hpp ---------------------------------------------------------
using OrderId = uint32_t;
inline constexpr OrderId kNoOrder = UINT32_MAX;

// The one signed helper every fill decision in every model goes through, so a
// direction error cannot hide in one branch (trap 15).
inline bool at_or_through(Side s, int32_t limit, int32_t px) {
    return s == Side::Buy ? px <= limit : px >= limit;
}
inline bool better_than_ours(Side s, int32_t limit, int32_t px) {
    return s == Side::Buy ? px > limit : px < limit;
}

// mbo only: ref -> recorded shares still ahead of us. Open-addressed, sized to the
// level's `count` at arrival. Decrement on a partial removal; erase only on a total
// one (trap 9). O(1) membership — never a vector scan.
class AheadSet {
public:
    void build(const book::Book&, Side, int32_t price, uint32_t hint);
    bool contains(uint64_t ref) const;
    void reduce(uint64_t ref, uint32_t by, bool total);
    uint64_t recorded_shares() const;               // for the conditional I3b
    void clear();
};

// Level-scoped counters live here, NOT on Entry: incrementing them inside the
// per-Entry loop multiplies every one of them by the number of our orders at a
// level, and `cancel_shares_ahead / cancels_seen` is a ratio the write-up quotes
// (trap: stats multiplied per entry).
struct Stats {
    uint64_t fills = 0, fill_shares = 0;
    uint64_t cancels_seen = 0, cancel_shares_ahead = 0;
    uint64_t exec_shares_at_our_levels = 0;         // the share-weighted at-touch ratio
    uint64_t cancel_shares_at_our_levels = 0;       //   numerator/denominator, 7.2
    uint64_t lock_fills = 0, lock_shares = 0, lock_dwell_ns = 0;
    uint64_t through_fills = 0, through_shares = 0;
    uint64_t hidden_ahead_shares = 0, hidden_at_price_shares = 0,
             hidden_inside_shares = 0;
    uint64_t naive_fills_from_hidden = 0, naive_shares_from_hidden = 0;
    uint64_t repriced_c = 0, nonprintable_c = 0;
    uint64_t priority_anomalies = 0, anomaly_shares = 0;   // mbo
    uint64_t clamp_events = 0, clamp_shares = 0;
    uint64_t unknown_ref = 0;
    uint64_t counterfactual_overruns = 0;           // a trade ate past us AND past our qty
};

// ---- sim/queue_model.hpp ---------------------------------------------------
enum class Model : uint8_t { Naive = 0, Optimistic = 1, Mbo = 2, Pessimistic = 3 };
inline constexpr int kModels = 4;
inline const char* to_string(Model);            // "naive" / "optimistic" / "mbo" / "pessimistic"

enum class CrossPolicy : uint8_t { Exclude, Ignore, FillAtCrossPrice };

enum class Trigger : uint8_t {
    Unset = 0,
    Execution,        // 'E'
    ExecutedAtPrice,  // printable 'C' whose print price == the resting price
    Lock,             // R1/R2: the opposite side reached our price
    Through,          // R3: a print strictly through our limit
    Hidden,           // 'P' — Naive only
    TapePrint,        // Naive only: a printable trade at-or-through our limit
};

struct SimFill {
    OrderId   order        = kNoOrder;
    uint64_t  ts           = 0;
    uint64_t  event_index  = 0;
    Model     model        = Model::Naive;
    Side      side         = Side::Buy;
    Liquidity liq          = Liquidity::Unset;   // Unset is a poison value, checked
    int32_t   price        = 0;                  // our limit; no price improvement modelled
    uint32_t  shares       = 0;
    uint64_t  ahead_before = 0;
    Mid       mid;                               // PREVAILING: the batch-start mid
    Trigger   trigger      = Trigger::Unset;
};

// One message, resolved against the PRE-mutation book.
struct Resolved {
    char     type = 0;
    uint64_t ts = 0, event_index = 0;
    bool     known = true;              // false: the reference was not in the book

    char     rem_side  = 0;             // the level shares were REMOVED from
    int32_t  rem_price = 0;             // the RESTING price — never C's wire price
    uint64_t rem_ref   = 0;
    uint32_t removed   = 0;             // min(wire shares, o->shares), as reduce() clamps
    bool     total_removal = false;     // mbo: erase the set entry only when true

    char     add_side  = 0;             // A, F, and U's add half
    int32_t  add_price = 0;
    uint32_t add_shares = 0;
    uint64_t add_ref   = 0;

    bool     is_execution       = false;   // E, printable C, non-printable C
    bool     is_printable_trade = false;   // E, printable C, P, Q
    bool     fills_as_trade     = false;   // E, or printable C with print == resting
    int32_t  print_price        = 0;
    uint64_t print_shares       = 0;
    uint64_t match_number       = 0;       // PER-EXECUTION. Does NOT group a sweep.
};

inline Resolved resolve(const book::Book& pre, char type, const uint8_t* p,
                        uint64_t event_index);

class QueueModel {
public:
    struct Config {
        Model       model              = Model::Pessimistic;
        bool        clamp_to_level     = true;    // see §2.4; report both curves
        bool        naive_hidden       = true;    // 'P' — report the attributable share
        CrossPolicy cross              = CrossPolicy::Exclude;
        bool        continuous_only    = true;
        uint32_t    max_pct_of_level   = 20;      // refuse to place beyond this
        uint32_t    lock_scan_levels   = 8;       // depth walked for R1
    };

    struct Entry {
        Side     side = Side::Buy;
        int32_t  price = 0;
        uint32_t display = 0, hidden = 0, original = 0, filled = 0, cancelled = 0;
        uint64_t ahead = 0, ahead0 = 0;
        uint64_t arrived_ns = 0;
        bool     live = false, locked_now = false;
        AheadSet ahead_set;                        // Mbo only
        // per-ORDER counters; level-scoped counters live on QueueModel so that N of
        // our orders at one level do not multiply them by N
        uint64_t cancels_seen = 0, cancel_shares_ahead = 0;
        uint64_t exec_shares_at_level = 0;
    };

    explicit QueueModel(Config c = {}) : cfg_(c) {}

    OrderId place(uint64_t arrival_ns, Side, int32_t price, uint32_t qty,
                  uint32_t display, const book::Book&);
    void    cancel(OrderId, uint64_t now);
    void    amend_down(OrderId, uint32_t new_qty);             // keeps position ('X')
    void    replace(OrderId, int32_t px, uint32_t qty,
                    const book::Book&);                        // to the back ('U')

    void commit(const Resolved&, const book::Book& post);
    void step(book::Book&, char type, const uint8_t* p, uint64_t event_index);

    uint64_t shares_ahead(OrderId) const;
    uint32_t remaining(OrderId) const;
    const std::vector<SimFill>& fills() const;
    const Stats& stats() const;
    bool audit(const book::Book&) const;    // I1, I2, I3, I6
};

// Four lanes, one resolve(), one book. Same OrderId in every model.
class ModelBank {
public:
    explicit ModelBank(QueueModel::Config base);
    OrderId place(uint64_t arrival_ns, Side, int32_t px, uint32_t qty, uint32_t display,
                  const book::Book&);
    void    cancel(OrderId, uint64_t now);
    void    step(book::Book&, char type, const uint8_t* p, uint64_t event_index);
    const QueueModel& model(Model) const;
    bool    containment_holds() const;      // I4, I5
};

// ---- sim/ledger.hpp --------------------------------------------------------
class Ledger {
public:
    void apply(const SimFill&, const FeeSchedule&);

    int64_t position() const;
    Money   cash() const;
    Money   exchange_fees() const;          // + = net paid, - = net received
    Money   regulatory_fees() const;
    Money   unattributed_cash() const;      // fills with no usable mid at the instant
    uint64_t unattributed_shares() const;

    Money equity(int64_t two_mark) const;   // cash + position * mark
    Money edge() const;                     // spread capture
    Money drift(int64_t two_mark) const;    // adverse selection
    // equity == edge + drift + exchange_fees + regulatory_fees, exactly.
    Money break_even_maker_rate(int64_t two_mark) const;   // + = can afford to PAY
};
```

### 8.4 CLI

```
queue_backtest <feed.gz> [--symbol SYM]
  --strategy touchmaker|crosser|farquoter|flicker
  --size N --offset-ticks N --requote-policy hold|touch-move|interval --requote-ns N
  --max-position N                          harness-enforced, not strategy-enforced
  --model naive,optimistic,pessimistic,mbo  default: all four
  --mode frozen|free                        default: frozen (the headline)
  --unclamped-pessimistic                   report the fifth diagnostic curve
  --latency colo_fpga|colo_sw|metro|retail|symmetric
  --feed-ns N --think-ns N --order-ns N --cancel-ns N --ack-ns N
  --tie market-first|ours-first
  --cross-policy take|reject-back|slide      arrival-marketable behaviour
  --fee-file PATH --maker-micros N --taker-micros N
  --markout-ns 100ms,1s,10s[,300s]
  --naive-hidden / --no-naive-hidden
  --quote-stop HH:MM:SS --flatten-by HH:MM:SS
  --json PATH --fills PATH --equity PATH --markout-csv PATH --interval-ms N
  --limit N --end-ns N --utc-day D --tick N
  --verify --strict --allow-zero-latency --seed N --quiet

exit: 0 ok | 1 error | 2 usage | 3 invariant violated (--verify)
```

`--interval-ms` (matching `book_replay`, whose field is `interval_ns`) samples the
equity curve on the **snapshot grid**: multiples of the interval anchored at midnight,
first grid point after the first message — `book_replay.cpp`'s exact rule, so the two
CSVs line up.

Every output artifact — JSON **and** CSV — carries a provenance header: feed filename,
SHA-256, byte count, symbol, date, `itch_census` histogram, strategy and its
parameters, model set, latency channels, tie policy, fee schedule **name and effective
date**, session bounds (both `continuous_session_end_ns` and the replay window bound),
mid convention, exclusion counts, and the seed. Two runs' outputs are otherwise
indistinguishable after the fact, and the numbers become uninterpretable — exactly the
failure the design warns about elsewhere.

### 8.5 Charting, dependency-free

`python/analysis/fill_comparison.py` writes SVG directly from the Python standard
library (`<polyline>`, `<rect>`, `<text>` emitted as text) to `docs/fill_comparison.svg`,
and **always** writes the source data alongside to `docs/fill_comparison.csv`. It also
has `--ascii`, which prints the same comparison as a markdown table.

`python/analysis/` is pure stdlib today (`validate.py` imports `databento` only inside
a function body) and CI installs nothing but cmake/ninja/zlib/clang. Adding matplotlib
would put `pip install` into the reproduction instructions, and "reproduction
instructions that actually work on a clean machine" is itself a Phase 8 deliverable.
SVG is a text file, so the chart diffs in git. `bench/README.md` already establishes
that the primary record is a markdown table and the visual is a bonus; `--ascii` keeps
that true and lets CI assert on the chart's contents.

The chart caption carries symbol, date, strategy, size, requote policy, latency
regime, fee schedule and mode. A committed SVG is an assertion nobody can check
otherwise.

### 8.6 Data hygiene

Commit only derived artifacts: `validation/queue_MSFT_2019-12-30.json`,
`docs/fill_comparison.csv`, `docs/*.svg`, and the provenance stanza. Never the feed,
never a slice — `.gitignore` already blanket-ignores `*.gz`. The SHA-256 is what lets a
reader with their own copy of `12302019.NASDAQ_ITCH50.gz` confirm they are looking at
the same bytes before comparing numbers.

One correction to make as part of this: `python/slice_symbol.py`'s docstring says a
per-symbol slice is "small enough to commit and hand to someone else." The feed is
licensed and was deliberately purged from git history; that line invites exactly the
mistake this phase must not repeat.

---

## 9. How this gets tested

Five layers, mirroring the strategy that already worked for phases 2–5. Layer 2 is the
load-bearing one: a counterfactual cannot be validated against reality, but a
counterfactual *computation* absolutely can be validated against an independent
implementation of the same specification, and that is what breaks.

| layer | mechanism | catches |
|---|---|---|
| 1 | hand-computed micro-scenarios, `tests/check.hpp` | per-message rules, the two arithmetic rules, sign errors |
| 2 | `python/reference/queue_sim.py` + `tests/queue_differential.py`, byte-identical fill logs | logic divergence, the pre/post-mutation ordering trap |
| 3 | ~~per-event invariants inside the driver under `--verify`~~ **NOT BUILT** | containment, monotonicity, conservation |
| 4 | ~~golden `--json` / `--fills` in `tests/golden/`, diffed in CI~~ **NOT BUILT** | silent regressions |
| 5 | **the delete-one-order oracle** (§9.3) | the fill rule itself, against ground truth |

**Layers 3 and 4 were not implemented.** There is no `--verify` flag on any tool
and no `tests/golden/` directory, so §9.1's "roughly 4.6M invariant checks on the
MSFT day" never ran. What covers that ground instead:

* Layer 3's containment/monotonicity/conservation properties are checked by
  `tests/fuzz/fuzz_queue.cpp` over random sequences, and by the clamp and
  priority-anomaly counters, which are reported on every run rather than
  asserted — see `docs/phase6-results.md` §2. That is weaker per event and
  broader per input than what was designed.
* Layer 4's regression role is served by `tests/queue_differential.py`, which
  requires byte-identical fill logs between the C++ and Python simulators. A
  golden file pins one recorded output; a differential pins two independent
  implementations against each other, which is the stronger of the two, but it
  does not catch a regression that moves both.

Recording the gap here rather than quietly dropping the rows, because a design
document that lists five test layers and ships three is exactly the kind of
claim this project is supposed to notice.

### 9.1 The invariants

**As designed** (not built — see the note above the table): `--verify` would
check these per order, per event, roughly 4.6M times on the MSFT day across four
lanes, and free relative to the gzip decode and book reconstruction. The list
below is still the right list; it is the enforcement mechanism that is missing.

| | invariant |
|---|---|
| **I1** | `0 <= ahead <= shares_at(S,P) + own_live_shares_ahead` after every message (clamp on) |
| **I2** | `ahead` never increases except at `place` / `replace` / iceberg refresh |
| **I3** | `shares_at(S,P) + own_live_shares_ahead == 0  ⟹  ahead == 0` |
| **I3b** | `mbo`: `ahead == Σ recorded shares over the ahead-set` — **conditional** on no priority anomaly since arrival and no clamp having fired. Two mechanisms, no shared code. |
| **I4** | `ahead_optimistic <= ahead_mbo <= ahead_pessimistic`, pointwise |
| **I5** | cumulative filled shares: `naive >= optimistic >= mbo >= pessimistic`, per order, per event |

**I4 and I5 carry two conditions, both found by the fuzzer rather than by
reading.** Each is a real limit on the containment argument, not a bug:

1. **No lane may have refreshed an iceberg slice.** The argument is inductive —
   every lane starts at the same `ahead0`, and optimistic's decrements are a
   superset of `mbo`'s, which are a superset of pessimistic's. A refresh *resets*
   `ahead` from the book instead of decrementing it, and because the faster lane
   fills first it also refreshes first, and is then sitting behind a queue the
   slower lane has not rejoined yet. Observed directly:
   `opt(ahead=93, refreshes=1)` against `mbo(ahead=1, refreshes=0)`.
   Consequence: **the published band brackets `mbo` only for fully displayed
   orders.** For an iceberg, the bounds are still individually meaningful but
   their ordering is not guaranteed.

2. **At most one live order of ours per `(side, price)`.** The clamp's limit
   includes our own earlier orders' *remaining displayed* shares, and that
   remainder is model-dependent because each lane fills it at a different rate.
   The limit then differs per lane and containment breaks — observed as
   `mbo(ahead=65)` against `pess(ahead=13)`, an inversion of the bound itself.
   This is why the reference strategy asserts one live order per price (section
   3.6); it is a correctness constraint, not tidiness.

`Entry::refreshes` exists so a property test can tell a legal increase from a
bug, rather than inferring it from share counts.
| **I6** | per order: `filled + cancelled + display + hidden == original` at all times; at session end `display == hidden == 0` and `position == 0` |
| **I7** | our filled shares at a price level over any window `<=` (executed shares at that level and at prices through it on our side) + (R1/R2 reach consumed) over the same window |
| **I8** | re-deriving realised P&L and position from the fill audit record equals the incrementally maintained ledger, **exactly**, in micro-dollars |
| **I9** | `50 · Σ_i q_i · two_edge_i == Ledger::edge()` and `50 · Σ_i q_i · two_drift_i(terminal) == Ledger::drift(terminal)`, over a reconciled exclusion set |
| **I10** | no fill of ours carries a timestamp earlier than `decision_ts + think + order`; `ahead0` is snapshotted at **arrival** |
| **I11** | every channel at zero ⟹ the exchange book and the delayed view emit byte-identical snapshot CSVs (`python/analysis/book_diff.py`) |
| **I12** | two runs of the same feed with the same seed produce byte-identical fill CSVs and JSON |
| **I13** | Mode B: the four lanes consume a byte-identical intent tape (hash per lane, assert equal) |
| **I14** | the taker path never mutates the feed book: `Book::unknown_ref()` and `resting_shares()` match a control run with no strategy |

**I5 is provable, but only with every correction in §2 and §3 in place.** By induction:
`ahead_opt <= ahead_mbo <= ahead_pess` pointwise (each advances on a superset of the
others' events, from the same `ahead0`, under the same clamp), so the overflow ordering
reverses; and a model whose cumulative fills already equal the order quantity cannot be
overtaken. `naive >= optimistic` requires that non-printable `C` and repriced `C` never
fill *any* model (so no queue model can fill where naive does not) and that naive's
session gate is identical.

Written naively — with non-printable `C` filling the queue models but not naive, or
with `mbo` declining to advance on a trade against a post-arrival reference —
**I5 and I4 both fire on correct code**, and the natural response under deadline
pressure is to "fix" the model until the invariant holds. That is how a backtester
lies. The invariants are stated here in the form that is actually true.

**What is deliberately NOT asserted:**

* **P&L ordering across models.** More fills is not more money, and Phase 6 exists
  precisely because it is not. A passive quote filled more often in a falling market
  loses more — that is the definition of adverse selection, the thing this phase
  measures. For the knowingly unprofitable strategy the P&L ordering inverts. An
  assertion here would eventually fire on correct code, and worse, if it *never* fired
  it would be evidence the adverse-selection measurement is broken. P&L ordering is a
  **result**, reported, never asserted.
* **Latency dominance.** False in both directions. More latency delays cancels too, so
  you cannot pull in time and get *more* fills — generally bad ones. And arriving later
  can mean joining a level that has since cleared, i.e. a *better* queue position. **I10**
  is the ironclad, cheap property that catches the bug which actually happens: reading
  the book at decision time instead of arrival time.
* **Grouping fills by match number.** ITCH match numbers are per-execution — the key
  the Broken Trade message uses to bust an individual trade — so "our fills within a
  match group never exceed the group's executed shares" is true by construction and
  tests nothing. **I7** is the falsifiable replacement.

### 9.2 Generators, and which layer each feeds

The existing generators are correct for their stated purposes and wrong for this one.
`make_bench_feed.py` reproduces a real day's message **mix** for throughput
measurement; `fuzz_feed.py` is deliberately adversarial and structurally illegal. What
neither provides:

* Executions never hit the **front of the price-time queue**: both draw the target
  reference with `pick_live()` from a global pool across all prices and ages, so an
  `E` can hit the middle or back of a level's FIFO 800 ticks from the touch. There is
  no queue structure to preserve, so a correct queue model is indistinguishable from
  one that advances on arbitrary references.
* Cancels are drawn from the same uniform pool, so "where in the queue do cancels come
  from" — precisely what `mbo` measures — is baked in as uniform, and `mbo` lands near
  the middle of the band **by construction**.
* Side and price are drawn independently around a fixed `MID`, so the generated book is
  crossed essentially everywhere. There is no touch, so "the market traded at your
  price" and "the mid" are not well defined, `mbo` is downgraded to approximate on
  every run, and layer-3 invariants cannot be evaluated at all.
* `MID` is a constant with no drift, volatility or autocorrelation, so mark-to-market
  P&L is a driftless random walk and **measured adverse selection is identically zero
  in expectation**.
* Timestamps advance by `rng.randint(1, 200)` — **strictly increasing, never a tie** —
  so `EventClass`, `TiePolicy` and `EventKey::seq`, which §4.4/§4.6 call load-bearing,
  have zero coverage from any feed this project can generate.
* `fuzz_feed.py` only ever emits `trading_action(t, b"T")`, so there is no halt
  sequence, no `'Q'` quotation-only state, and therefore **no test of the session gate
  at all**.
* Both price `C` with `pick_price()`, so essentially **100% of generated `C` messages
  are the repriced shape**. The untested case is the opposite one — a `C` at the
  resting order's price, which is the only `C` shape that triggers a fill. (Contrary to
  a natural assumption, non-printable `C` *is* well covered: `make_bench_feed.py` emits
  `printable='N'` 10% of the time and `fuzz_feed.py` 33%.)
* `Q` crosses are emitted at random prices, never at a price where a tracked order
  rests.
* No opposite-side add that locks and then crosses a tracked price — so R1/R2 are
  untested.

**`python/make_queue_feed.py`** — new, and its only claim is book-legality. Never
crossed (a new bid never exceeds the best ask); `E`/`C` always applied to the **head**
of the price-time queue at the traded price; `X`/`D` drawn per level with a
configurable `--cancel-front-bias`; burst-structured timestamps with a `--tie-burst`
mode emitting k messages at one nanosecond; a full `H('H') → H('Q') → Q(cross_type 'H')
→ H('T')` halt-and-resume sequence; a `Q` cross at the price of a tracked order,
preceded by non-printable `C`s at that same price for the participants; a `C` at the
exact resting price, printable and non-printable; an opposite-side `A` that locks and
then crosses a tracked price; `P` with side `'S'` as well as `'B'`. Its docstring must
state in the first paragraph that it establishes **mechanism** and proves nothing about
markets. `--cancel-front-bias` is what lets the tests force the models apart on demand
and force them to coincide (bias 0 with zero cancels collapses optimistic onto
pessimistic) — which is how you test the models rather than the generator.

**`python/make_toxic_feed.py`** — new, and it needs a **known answer** or it proves
nothing. A latent fair-value random walk; cancel intensity at a level rising as fair
value approaches from the other side; aggressor arrivals whose direction is a function
of `(fair_value − touch)`; and — the part that actually tests the models — a
**ground-truth queue position written to a side-channel file**, so the optimistic and
pessimistic brackets can be validated against the true `ahead` rather than merely
against each other. An informed sweep that consumes k levels and then moves the mid by
exactly d ticks with probability p yields a closed-form expected 100 ms drift the test
asserts to within the block-bootstrap standard error. Without a known-answer generator,
a green suite proves only that we computed zero correctly.

**Layer pairing, stated so it cannot be got wrong:** layer 2 (the differential) runs on
`fuzz_feed.py`, because that is what exercises unknown references, crossed books and
off-grid prices. Layer 3 (`--verify` invariants) runs on `make_queue_feed.py` **only**,
because the invariants are undefined on a book that is crossed everywhere. CI runs both.

### 9.3 The external oracle — delete one order from the feed

This is the strongest test available and it needs **no licensed data**. It is the
Phase 6 analogue of the Phase 3 differential test, and without it this phase grades its
own homework by the project's own standard.

1. Pick a real resting order `R` in any feed (synthetic or real) with reference `r`,
   arrival time `t_a`, price `P`, side `S`, size `Q`, and a known fate — fully executed,
   partly executed, or cancelled. Follow the `U` chain if `r` is replaced.
2. Produce a **counterfactual feed** with every message naming `r` (and its replacement
   chain) removed: its `A`/`F`, and all of its `E`/`C`/`X`/`D`/`U`.
3. Run `queue_backtest` on the reduced feed, placing **our** order at `t_a`, `P`, `S`,
   `Q`, at zero latency.
4. Grade the sim's predicted fills against `R`'s **actual** `E`/`C` messages in the
   unmodified feed.

This grades the fill rule, the `ahead` arithmetic, the trade-vs-cancel split, the sweep
composition, and the R1/R2/R3 price-priority rules — all against ground truth. It also
directly measures how much the removal perturbs the rest of the day, which is the
counterfactual-divergence number the design otherwise only warns about qualitatively.

Report, over a large sample of removed orders: the fraction where the true fill time
falls inside `[pessimistic, naive]` (**the band's coverage rate — the single most
publishable number in the phase**), the fraction where `mbo` predicts the fill time
exactly, and the distribution of the error.

`tools/` gains a small `--delete-ref R` mode on `itch_slice`, or a Python
`python/drop_ref.py`, to produce the counterfactual feed.

### 9.4 Unit tests that must exist

Each is a `tests/check.hpp` file of a few dozen lines, and each maps to a rule above
that would otherwise be asserted and never exercised:

* `Channel::arrival()` clamping (non-decreasing, not `last+1`) and saturation.
* The tie-break table: one hand-built `--tie-burst` feed per `TiePolicy`, asserting
  different fills.
* The four cancel-race buckets, driven by a feed with an `E` placed at a chosen
  nanosecond inside the cancel's flight window.
* `queue_delta` against a feed with a known `A` for 500 shares at our price inside the
  flight window: assert `ahead_at_arrival == ahead_at_decision + 500`.
* **Two of our own orders at one level**: assert combined fills never exceed the level's
  traded shares, and that the clamp does not delete our own earlier order from `ahead`.
* `ahead 100, ours 200, one E for 300` → fill 200. `ahead 100, ours 200, one D for 500`
  → fill **0**, `ahead` 0. (Trade-class vs cancel-class, the double-count bug.)
* Unsigned underflow: `removed − ahead` in every one of its five instances, under UBSan.
* Iceberg: fill capped at `display`, refresh resets `ahead` to the back.
* A synthetic feed of nothing but `P` prints at the quote: **zero fills in the three
  queue models**, non-zero only in naive with `--naive-hidden`.
* Fee goldens at MSFT's real price: the regulatory cost of a 100-share **sell** at
  `1575700` Price(4) against the hand-computed micros (this is the test that catches an
  8× Section 31 error), the sub-$1 notional switch at the boundary price, the TAF cap
  at exactly 50,000 shares, the sells-only asymmetry, and the inverted-venue sign.
* A horizon expiring inside a same-nanosecond batch (the strict `<`).
* Each markout exclusion reason hits its own counter and none reaches the mean.
* A fill at 15:59:59.995 with a 10 s horizon is `PastSessionEnd` **even when the replay
  window runs to 19:00**.
* A run with zero `'S'` messages fails loudly rather than producing a naive-only chart.
* The **losing control**: `Crosser` must produce `net_pnl < 0` in all four models under
  every fee schedule. A backtester that cannot produce a loss is broken.

### 9.5 CI

Extend `.github/workflows/ci.yml`:

* Generate `make_queue_feed.py` and `make_toxic_feed.py` output.
* Run `queue_backtest --verify --strict` on the queue feed (exit 3 on any invariant).
* Run `queue_differential.py` against `python/reference/queue_sim.py` on `fuzz_feed.py`
  output.
* Run the backtest **twice on the same feed and diff** — this is what catches
  unordered-container iteration reaching the output.
* Diff the goldens.
* Run the test suite in **both Debug and Release**, so the Release behaviour — the one
  the published numbers come from, with `assert` compiled out — is what is tested.

**Determinism is a standing requirement, not a one-time fix.** Any `std::unordered_map`
iteration whose order reaches the output, any wall-clock read, or any unseeded RNG on
the decision path silently invalidates the frozen-decision comparison. `Matcher` already
uses `unordered_map` — harmlessly, for a summation whose order does not matter — so the
pattern is in the codebase and could be copied into the sim where it would not be
harmless. Rule: every aggregate that reaches JSON or CSV is accumulated in a `std::map`
or a sorted vector keyed by order id, and the four lanes live in a `std::tuple` visited
with `std::apply`, not in a container whose iteration order is incidental.

---

## 10. What requires real data — stated plainly

No feed is checked in. It is licensed and was deliberately purged. This section is the
honest accounting of what that costs, and it belongs in `docs/phase6-results.md` as two
separate tables — one headed **"mechanism (synthetic — proves the code works, proves
nothing about markets)"** and one headed **"measurement (real feed, SHA-256 recorded)"**
— never mixed.

### 10.1 Verifiable with synthetic data alone

Every invariant **I1–I14**. Every hand-computed micro-scenario. The C++/Python
differential. **The delete-one-order oracle** (§9.3), including the band's coverage
rate on synthetic flow. The full CLI, the JSON/CSV formats and their provenance
headers. The SVG generator. The demonstration that the four models *can* differ, and
that they collapse when they should (S7). The fee goldens. The losing control. The
latency mechanics: clamping, saturation, the tie table, the cancel-race buckets,
`queue_delta`, and the zero-latency book equivalence.

### 10.2 Requires the real feed, and cannot be faked

* **The width of the optimistic–pessimistic band.** It is a function of the real
  cancel-to-execution ratio *at the touch, share-weighted, while our order rests there*.
  Synthetic generators set that ratio by fiat.
* **Where `mbo` sits inside the band.** This is a behavioural fact about real
  participants — front-of-queue orders are valuable and rarely pulled, stale
  back-of-queue orders are pulled first, so real cancels are strongly position-biased.
  `pick_live()` bakes in the uniform answer, so on synthetic data `mbo` lands near the
  middle **by construction** and the number is meaningless.
* **Every adverse-selection number.** Synthetic feeds have no information content: the
  price process is independent of order flow, so measured drift is ~0 in expectation.
  `make_toxic_feed.py` can demonstrate that the machinery *detects* adverse selection
  when it is present, with a known answer — it cannot tell you the real magnitude.
* **Whether any strategy makes money.** Including the sign of the headline table.
* **Absolute fill rates**, and the requote-policy sensitivity.
* **The priority-anomaly rate** — how far `mbo` is from truth.
* **The hidden-flow interception rate** (`hidden_inside_shares`), the flagship
  adverse-selection signal.
* **The `P` Buy/Sell census.** Both generators hardcode `'B'`; whether the real field
  carries usable information is unknown until it is counted, and no rule may depend on
  it before then.
* **Whether the MSFT book locks or crosses intraday during continuous trading.** Nobody
  has checked: `book_replay` prints `crossed()` exactly once at end of run, with 657
  resting orders left at 19:00, and the only `INVARIANT(!m.crossed(), ...)` lives in
  `fuzz_matcher.cpp` and runs on synthetic matcher sequences. Adding an intraday
  continuous-session check to `book_replay` is a one-line change and a real result.

### 10.3 Cannot be resolved by any amount of data

These are the honest ceiling on the whole phase and belong in the **first paragraph**
of the results doc, not a footnote.

1. **Counterfactual divergence.** Our order was never in the real queue, so every
   execution the feed shows was matched against someone else. Had we been resting there,
   we would have absorbed shares that in reality went to orders behind us, and those
   participants' subsequent cancel and replace decisions would have differed. The sim
   counts the events where this becomes visible — an execution that eats past us, a
   taker fill that double-spends displayed depth — but it cannot correct for them.
2. **Zero market impact.** In a replay we are a ghost: our size never displaces anyone,
   and nobody ever reacts to our quote. This biases the backtest **optimistic in a
   direction the queue bounds do not capture**. The quote size is 100 shares against a
   1,000–5,000-share touch precisely to make the assumption as defensible as possible,
   and the sim warns (and optionally refuses) when quantity exceeds
   `max_pct_of_level` of the level's displayed size at arrival. The backtest is only
   honest for orders small relative to the queue, and there is no way to make it honest
   for large ones short of a full counterfactual replay.
3. **One venue, no routing.** NASDAQ TotalView is one book. The same parent order may be
   routed elsewhere; the NBBO may be set by another venue; a Rule 605 comparison uses a
   consolidated mid we do not have.
4. **`mbo` is exact only with respect to displayed liquidity.** Display-price sliding,
   non-displayed ranked interest, and routed-away-and-returned orders are invisible to
   it. `priority_anomalies` measures the rate; it does not remove the bias.
5. **Broken Trade (`'B'`) messages are not decoded.** `tools/itch_census.cpp` already
   names the type; the parser does not decode it. A busted trade means a fill the model
   recorded did not happen, and the match number is exactly the key ITCH provides for
   correlating it. Stated as a known hole, not silently ignored.
6. **The Rule 605 comparison is an order-of-magnitude and sign check**, not an oracle
   (§5.7). Presenting it as equivalently strong to the Phase 2 Databento bar would
   overclaim.
7. **Statistical power on one day.** Even the market-wide cohort cannot resolve a
   0.3 c/share 10 s effect on a single day (§5.6). The 100 ms markout can be resolved;
   the 10 s markout cannot. Publishing a single 10 s number without its block-bootstrap
   confidence interval is the most likely way this phase produces a confident wrong
   claim.
8. **Single-symbol, single-day markouts are contaminated by market-wide moves.** A 1%
   index move during the session dominates the 10 s bucket and has nothing to do with
   our fills. De-meaning against a market factor needs a second instrument in the
   replay, which the current single-symbol slicing pipeline does not provide.

### 10.4 The rule for the README

**No number in the README came from a generator whose parameters we chose.** The
published chart comes from a named real day with its SHA-256 recorded, and the
reproduction path requires the reader's own ITCH file. Synthetic results are labelled
as mechanism demonstrations, in their own table, on their own page.

---

## 11. Traps — errors the critiques caught, recorded so they are not reintroduced

Every one of these was in a design that read as reasonable. Most of them bias P&L
**upward**, none of them crash, and several would produce a confidently wrong number
that looks plausible. Grouped by where they live.

### Queue mechanics

1. **Same-side-only fills.** A model that fills only on `E`/`C` at our exact `(side, price)`
   can never fill when the *other side comes to you* — and for an order that improved
   the touch there is by construction no real order at our level, so it can never fill
   at all. Every omitted fill is an adverse-selection fill: the bias is one-directional
   and flatters P&L. **Fix:** R1/R2/R3 in §2.5, applied identically in all four models
   because price priority, not time priority, decides them.

2. **Non-printable `C` filling in the cross.** "The repriced-`C` rule keeps us out of
   the auctions" is **false** whenever the cross price equals the resting price — which
   is exactly the case for a participant resting at the cross price. On the validated
   day 2,500,408 of 6,154,278 shares (40.6%) crossed at two prices. The session gate
   does not save you either: `S = 'Q'` fires at 09:30 and the opening cross prints
   immediately after it, strictly inside the gate. **Fix:** one rule — a `C` fills only
   when `printable == 'Y'` **and** `print_price == resting_price`.

3. **Trading state `'Q'` omitted from the non-tradable set,** so halt-resumption crosses
   fill mid-session. And `trading_state_` initialises to `'\0'` and is only set by an
   `'H'` message, so a `!= 'T'` test drops everything until the first `'H'` arrives
   while an `== 'H'` test lets the whole pre-open and every paused state through.
   **Fix:** gate on `system_event()` first, then `trading_state() == 'T'`; `'\0'` is
   unknown-and-not-tradable and counted.

4. **The clamp deleting our own earlier orders.** `ahead = min(ahead, shares_at(S,P))`
   is wrong because our orders are not in `book::Book`. With two live orders at one
   price, the second one's `ahead` includes phantom shares the level total cannot
   account for, and the very next message at that level collapses it — producing exactly
   the two-order double-fill the design claimed to prevent. **Fix:**
   `min(ahead, shares_at + own_live_shares_ahead)`, with a two-order unit test.

5. **The clamp silently redefining "pessimistic".** Clamped-pessimistic is *not*
   "cancels never advance you". It is sound and it is the right default, but if the
   README says one thing and the code does another the headline range is not the range
   the write-up claims. **Fix:** report both curves and `clamp_events`/`clamp_shares`.

6. **Cancels spilling into fills.** One uniform "consume the queue, then fill from the
   remainder" path fills you off a large `D` at your level. **Fix:** trade-class =
   consume-then-spill; cancel-class = consume-and-discard, never fills. Two unit tests
   that differ only in the message type.

7. **Unsigned underflow.** `removed - ahead` evaluated before the comparison wraps to
   ~4e9 and reports a fill of the entire order. `Book::reduce` already documents this;
   the queue model has five more instances. Fails silently, not loudly. **Fix:** compare
   before subtracting, everywhere; a dedicated UBSan test.

8. **Asserting a total order on fills across all four models.** False in two documented
   directions in a naive design: pessimistic advances on a trade against a post-arrival
   reference where `mbo` would not, and non-printable `C` makes optimistic exceed naive.
   A fuzzer that fires on correct code gets "fixed" until it passes, which is how a
   backtester lies. **Fix:** the corrected rules in §2/§3 make **I4** and **I5** true;
   `mbo` advances on **all** executions and uses reference identity only for cancels.

9. **`mbo` erasing an ahead-set entry on a partial removal**, which drops the rest of
   that order out of `ahead` on the next event and makes `mbo` fill early. And linear
   membership on a `std::vector` — the same O(n) trap `level.hpp` warns about for
   `shares_ahead_of()`, reintroduced one paragraph later. **Fix:** decrement on partial,
   erase on total, O(1) open-addressed membership.

10. **`match_number` used to group a sweep.** It is per-**execution** — the Broken Trade
    key — and ITCH carries no field identifying the aggressor at all. Any invariant built
    on it degenerates to "our fill on one `E` does not exceed that `E`'s shares", true by
    construction, testing nothing. **Fix:** timestamp contiguity, stated as a proxy;
    **I7** replaces the vacuous invariant.

11. **Sweep-through under-fill.** When a sweep exhausts our level and walks to worse
    prices, capping our fill at our level's real depth systematically under-fills a
    front-of-queue order at a thin level — again in the adverse-selection direction.
    **Fix:** R3.

12. **`ahead` re-derived from the book after a partial fill,** which re-inserts us behind
    liquidity our counterfactual just traded through and makes a partial fill strictly
    worse than no fill. And re-derived on a touch change, which throws away the queue
    position the phase exists to measure. **Fix:** **I2** — `ahead` only ever decreases
    after arrival.

13. **Icebergs with no display/hidden split,** so `fill = min(overflow, remaining)` fills
    the entire reserve off one execution — the exact inverse of the property being
    demonstrated. **Fix:** cap at `display`; refresh internally; `QueueModel` owns its
    slices (`Matcher::refresh_iceberg(Meta&)` is private and driven from the maker-side
    fill path).

14. **Hidden liquidity described as sitting *ahead* of you at your own price.** Backwards:
    NASDAQ continuous priority is **price, then display, then time**, so non-displayed
    size at your price is *behind* you. What it actually costs you is flow interception
    at a better price. **Fix:** the three hidden buckets in §2.6, and the corrected
    explanation in §2.7.

15. **Sign confusion on "better" vs "through".** For a bid at `P`, hidden interest *in
    front* prints **above** `P`; a print *through* our limit is **at or below** `P`. They
    are on opposite sides. Getting them backwards makes naive fill on prints strictly
    worse than its own limit — a fabricated fill that inflates the naive curve. And a
    midpoint peg at `P + 0.005` is neither "at our price" nor "through", so a two-bucket
    scheme silently drops the flagship signal. **Fix:** one signed `at_or_through()`
    helper used by every fill decision; three hidden buckets.

16. **Looking up `P`'s order reference in the RefMap** (it is not a book reference;
    probing it inflates `unknown_ref()`, the run's data-quality canary), and **keying on
    `P`'s Buy/Sell indicator** (it is the hidden order's side, hardcoded `'B'` in both
    generators, and widely constant in real TotalView output).

17. **A price predicate on unknown references.** "Count it if it would touch our level" is
    uncomputable: `D`, `X` and `E` carry no price on the wire, which is 99.9% of the
    references it would need to be evaluated for. **Fix:** count every unknown reference;
    non-zero ⇒ the run is approximate and `--strict` refuses to publish.

### Latency and event ordering

18. **A message barrier mistaken for a time barrier.** "Your intent cannot take effect
    until the next message" lets a quote **step out of the middle of an ITCH sweep** —
    three `E` messages at one nanosecond, cancel released between legs. No exchange
    permits it, and it bites hardest at latency 0. **Fix:** atomic timestamp groups
    (§4.4), `MarketFirst` ties, and a non-zero latency floor.

19. **Zero latency as the default,** on all channels. The strongest single form of silent
    optimism available: react to the message that moved the market and win queue priority
    over everyone with real wire time. A causality check of `arrival >= decision + 0` does
    not catch it. **Fix:** `colo_sw()` default; `--allow-zero-latency` required to publish
    at zero.

20. **A `last_arrival + 1 ns` FIFO clamp on the feed channel.** `last_` moves forward by
    at least 1 ns per message, so in a burst the accumulated synthetic delay grows
    linearly with burst length — tens of microseconds at the open, larger than the first
    four points of the latency grid. It also breaks the zero-latency equivalence test.
    **Fix:** non-decreasing (`max(sent + draw, last_)`).

21. **Cancels on an independent channel FIFO,** which reorders them against submits on the
    same TCP session and manufactures a `cancel_ns < order_ns` race that is a modelling
    artefact. **Fix:** one shared session FIFO; the `cancel` delay knob survives, the
    reordering does not.

22. **`Outcome::Rested` as the aggregate-init default,** silently absorbing every
    `engine::Reject` path as a successful passive placement. A backtester whose default
    outcome is the successful one is the textbook silent-optimism failure. **Fix:** an
    `Unclassified` poison value, asserted overwritten; `RejectedByEngine` carrying the
    `Reject` code; per-reject counts in the report. Same for `Liquidity::Unset`,
    `Trigger::Unset`, and an unset `Model`.

23. **`MarketFirst` described as uniformly pessimistic.** It is optimistic for the cancel
    race it was justified by: a tying `X`/`D` applied before our arrival snapshot makes
    `ahead` *smaller*. And "a market cancel keeps its place ahead of us in the FIFO" is
    not coherent — a cancelled order leaves the queue. **Fix:** state it as mixed, report
    tie counts, offer `OursFirst`, and do not claim a bound that is not implementable
    without reordering the feed inside a nanosecond.

24. **An `EventKey` total order that no queue actually stores.** Merging four deques on
    bare timestamps cannot break the one case the ordering exists for. **Fix:** every
    queue stores an `EventKey`; one shared `seq_`; `drain_until` recomputes the minimum
    after each event.

25. **The feed/order split sweep, sold as a chart nobody else can give you.** It is flat
    by construction for an event-driven strategy: arrival depends on the **sum**, and
    staleness at decision is always exactly `feed`. **Fix:** §4.2 — keep it for timer
    strategies, and use its flatness as a determinism test.

26. **Post-only cancel-back as the default,** which deletes exactly the adverse fills the
    latency curve exists to reveal (a joined bid becomes marketable in flight only when
    the offer fell *to or through* it), with an effect that grows with N. Also conflating
    post-only with Reg NMS Rule 610 display-price sliding — unrelated mechanisms.
    **Fix:** `CrossPolicy::Take` default; the other two labelled as elections.

27. **`assert` for anything that matters.** No-op under `NDEBUG`, which is the Release
    build the published numbers come from. `engine::transition()` already has this
    problem. **Fix:** checked conditions with a defined outcome, gated on
    `-DITCHBOOK_SIM_CHECKS` (on for publishable runs), and CI runs the suite in both
    Debug and Release.

28. **Copying an arbitrary-length payload into a fixed buffer.** `itchbook::parse()` hands
    **every** framed message to the handler, and `itch::spec_length()` returns `-1` for
    unmodelled types, so the length prefix check is skipped for exactly those types and
    `len` is generator-controlled. ASan catches it in Debug; the sweep runs Release.
    **Fix:** gate on `book::modelled(type)` before enqueuing, plus an explicit length
    check. And add the `stock_locate` filter that `book_replay` has, **including its
    `type != 'S'` exemption**.

29. **`reaction_time()` called "tick to trade".** In the trade that means the
    in-NIC-to-out-NIC time of your own box, and this quantity includes both propagation
    legs. It reads as a domain error to exactly the audience the project targets.

30. **Attributing Phase 4's 63 cycles/msg to `think`.** That is `parse` + `book::apply` —
    feed ingestion, ~21 ns at 3 GHz — while the presets set `think` to 0.3–3 µs. Claiming
    "the 1.73× is worth $X/day" on that basis attributes a book-reconstruction speedup to
    a decision-loop latency it does not govern. **Fix:** measure the reference strategy's
    own `on_event`. (`bench::calibrate_cycles_per_ns()` exists — no open question there.)

### Accounting

31. **A reconciliation identity that is an algebraic tautology.** `cash + open_cost ==
    realised` holds for *any* value of `basis`, `fee`, or the closing/opening split,
    because both sides come from the same two accumulators and the markout engine never
    appears. It cannot detect a wrong basis, a sign error on the rebate, a
    maker/taker misclassification, a wrong mid, or a wrong position update. Shipping it as
    the phase's substitute for an external oracle is disqualifying by the build plan's own
    standard. **Fix:** **I8** and **I9** — cross-module, over a reconciled exclusion set.

32. **SEC Section 31 wrong by ~8× and by a year.** It is *notional*-based, so on MSFT at
    $157.57 the FY2019 rate of $20.70/M is **0.326 c/share** on sells, not the ~0.04
    c/share you get by reasoning about a $20 stock — plus TAF 0.012. Averaged over a round
    trip that is ~0.169 c/share against a 0.20 c/share base rebate. Folding it in flips
    the pessimistic bar from +0.08 to −0.09 and moves the break-even rebate from 0.12 to
    ~0.29 c/share, i.e. from "comfortably below the base tier" to "the top tier". A
    per-share-only `FeeSchedule` cannot even express it. **Fix:** a per-notional
    component, a fee golden at MSFT's real price, and a dated config with a cited source.

33. **A "mils" fee unit.** Tenths of a cent invites `maker = -20` reading as $0.0020 and
    meaning $0.020 — four times the entire gross edge of a one-cent spread, turning the
    headline chart into a chart of the rebate. **Fix:** micro-dollars only; every
    published rate is an exact integer; a unit test asserting a 100-share passive fill
    produces `-2000` micros.

34. **Regulatory fees leaking into the break-even maker rate,** because they are charged on
    maker sells but bucketed as "non-maker". ~0.17 c/share of error — larger than the gap
    between the base and top rebate tiers, which is the entire point of the table. And the
    return value's sign documented backwards against the schedule's own convention.

35. **A two-term attribution.** `equity == edge + drift` is short by the fee stream, so
    defining `holding = equity − edge` dumps every rebate and taker fee into the bucket
    labelled adverse selection. At a one-cent tick the rebate is 20% of the gross spread.
    **Fix:** three terms; gross and net printed side by side.

36. **`edge` claimed invariant across fill models.** The models fill at different times, so
    the same limit meets a different mid. And because our orders are not in the book, a
    passive fill can occur at a price on the wrong side of the mid, giving a **negative**
    edge — which the "positive by construction" comment asserts cannot happen.

37. **A `two_mid = 0` sentinel reaching the ledger.** `observe()` returns early for
    `OneSided`/`Empty` with `two_mid = 0`; one such fill mis-attributes its entire notional
    to spread capture with an equal and opposite error in drift, while `gross()` and
    `net()` stay correct so nothing looks wrong — and the tautological identity above
    cannot detect it. **Fix:** an accessor that refuses on an unusable status; an
    `unattributed_` bucket.

38. **The `two_mid` justification.** "A half-cent mid rounds and biases buys and sells by
    0.25 c/share" is wrong by ~50×: `Price(4)` has four decimals, so half a cent is 50
    units and is exact, and Rule 612 puts every displayed quote on the penny grid so
    `bid + ask` is always even. Keep `two_mid` — the reasons in §5.2 are real — but the
    published justification must be one a microstructure-literate reader will not catch.

39. **The prevailing mid taken mid-sweep.** Re-observing after every message stamps leg k
    of an N-leg sweep with a mid already walked down by legs 1..k−1, understating **both**
    effective spread and adverse selection for exactly the toxic events being measured —
    and using two incompatible definitions of the mid in one engine. **Fix:** batch-start
    mid.

40. **Horizon-endpoint exclusion called "unbiased by inspection".** It conditions the
    sample on the future, in the direction the design itself identifies as most adverse
    (books go one-sided when they are swept). **Fix:** drop at the fill instant, bracket at
    the horizon endpoint.

41. **`n_eff = (Σw)²/Σw²` as the confidence guard.** It corrects only for unequal share
    weights, not for overlapping horizons — which understates the 10 s standard error by
    ~4×, giving a CI narrower than the entire base rebate. A too-narrow standard error
    published as the guard against overconfident claims is worse than none.

42. **Rule 605 sign errors** (effective spread is `+2·edge` from the resting side, realized
    spread is `2·markout(300s)` not `2·drift`) **and overclaiming its strength** — it uses
    the consolidated BBO at order receipt over a small non-random covered-order
    population, monthly, by size bucket.

43. **Marking the residual at the closing cross.** It is forward-looking (the cross prints
    at 16:00), you cannot reach it from 15:58 (the MOC deadline was 15:50), and it
    contradicts the decision to exclude our orders from auctions. Also
    `Book::cross_prices()` returns a **const** map, so `operator[]` does not compile and
    unguarded `.at()` throws on a short slice.

44. **`session_end_ns` name collision.** `validation/MSFT_2019-12-30.json` records
    `68400000000000` = 19:00 ET — the UTC-day window boundary from Phase 2, not the 16:00
    close (`57600000000000`). Reusing it means `PastSessionEnd` never fires near the close
    and 10 s markouts resolve against the after-hours book.

45. **`Book::crossed()` counts a locked book as crossed** (`return b >= a;`), so
    "`MidStatus::Locked` is usable" contradicts an existing invariant. And the **pre-open
    book is legitimately crossed** — that is the opening imbalance — so a "non-zero crossed
    count means the reconstruction is broken" canary fires a false alarm on correct data
    unless the phase gate runs first. Nobody has ever checked whether the MSFT book crosses
    *intraday*; `book_replay` prints it once at end of run.

46. **`fill_rate = fills / orders`** counts fill *events*, so it exceeds 1.0 — most often
    in the models with the *best* queue position, i.e. worst exactly where the chart cares.
    **Fix:** `filled_shares / submitted_shares`, plus a separate `orders_filled / orders`.

47. **Percentiles conditioned on filling.** `queue_ahead_at_join` and `time_to_fill`
    computed over fills compare different populations across models with 10× different
    fill rates. **Fix:** report `ahead0` unconditionally over all submitted orders (it is
    model-invariant in a single-pass design — a free self-check), and print the fill rate
    beside every conditioned percentile.

### Harness, strategy, and framing

48. **A `strategy_fill_independent` bool.** A position limit is by definition a function of
    fills, so a strategy enforcing its own cap cannot be fill-independent — and in a
    single-pass design with one order stream the flag is unfalsifiable. **Fix:** the
    `HeadlineStrategy` concept with no `Ctx` and no fill callbacks (a fill-dependent
    strategy cannot compile), harness-enforced risk, and tape hashing (**I13**).

49. **A frozen-decision tape that carries intents but not risk state,** so replaying the
    pessimistic lane's tape into the naive lane gives it far more fills with none of the
    cap-driven cancels — unbounded inventory, reintroducing the Δinventory artefact the
    flatten policy exists to remove.

50. **No session *start* gate.** The file carries orders from 04:00 and `modelled()`
    accepts them, so a quoter runs through the entire pre-market and through the
    09:28–09:30 accumulation window where the book is **legitimately crossed by
    construction**. Most of the churn and message-rate counters come from liquidity nobody
    trades against.

51. **`pos == 0` claimed but not enforced.** Flattening without a harness-owned
    quote-stop lets a strategy still quoting at 15:59 be passively filled after the flatten
    and end the day with inventory the headline claims cannot exist.

52. **Reusing `Matcher::submit()` against a feed book.** `match()` breaks out on any maker
    it has no `Meta` for — every order the feed put in the book — so it trades **zero**
    shares, marketable limits fall through to `rest()` and are added at a price through the
    offer (making `crossed()` permanently true), `Market` orders pass `has_liquidity()` and
    match nothing, and `FOK` passes `available()` (which sums external depth) and then
    cancels. Nothing asserts. **Fix:** §6.6.

53. **Injecting our orders into the feed book.** Feed messages address orders by ITCH
    reference, so **nothing can ever remove them**: they are immortal for the rest of the
    day, inflating `Level::shares` (which is what `shares_at()` reads), able to become and
    hold the touch, and by mid-session the touch is a wall of our own zombie quotes.
    Meanwhile any taking path that calls `Book::take()` deletes real orders and bumps
    `unknown_ref()`, destroying the run's only data-quality signal. **Fix:** §1.2.

54. **`Ctx::take()` against a shared unmutated book,** so the same 100 resting shares are
    sold to all four lanes *and* to the real aggressor — four-way double-spending, worst at
    a forced end-of-day flatten, largest for the lane with the most inventory. **Fix:**
    §6.6 — state the assumption, cap participation, report the taker share of volume.

55. **`resolve()` walking the ref map a third time.** `find_index` is exposed in `book.hpp`
    *specifically* so callers do not walk the chain twice on the 52% of messages that are
    `E`/`C`/`X`/`D`/`U`. **Fix:** `apply_ex()`.

56. **`Quote` with no sizes, and `top_changed` documented as detecting size changes.** The
    type cannot provide it, and `Quote` has no `operator!=` either — so the comparison does
    not compile and the strategy never re-evaluates when the touch drains from 5,000 shares
    to 100 at the same price.

57. **`--requote-ns 1ms` as a default.** A quote that rejoins every millisecond never
    accrues queue position, so `mbo` collapses onto pessimistic for a reason that has
    nothing to do with the market. The requote policy is a first-class parameter of the
    finding, not a default.

58. **A far-out "null quoter" at `Price(4) = 1`.** That is $0.0001 — a sub-penny price,
    illegal under Rule 612 for a $157 stock, rejected by our own tick check, so the test
    silently has one working order instead of two. And both such prices sit on empty levels,
    so the test exercises the degenerate `ahead == 0` path, not the queue machinery it
    claims to.

59. **Asserting a `Flicker` fills zero times at zero latency.** Contradicted by the release
    rule: the order is live for one full fill-model pass. Assert the exposure window
    instead, and assert the count is **non-zero** — a `Flicker` that never fills means the
    window is being skipped.

60. **`Crosser`'s "exact to the micro-dollar" loss formula.** `−(ask − bid)` at a single
    instant ignores the price drift between the two legs (MSFT ran 159.20 → 156.73 that
    day), and no exact assertion is possible against the *decision-time* touch when the IOC
    arrives later. **Fix:** exact accounting from the audit record + a genuinely a-priori
    loss bound.

61. **FIFO among our own orders claimed for naive.** Naive has no queue, so two of our
    orders at one price both fill against one print — 200 shares against a 100-share trade,
    violating **I7**. **Fix:** one live order per `(side, price)` in the reference strategy;
    oldest-first allocation otherwise.

62. **Calling the auction exclusion "conservative".** It removes 40.6% of the day's volume
    and its P&L sign is arbitrary. Say "excluded — not modelled", and print the number.

63. **`Level::shares_ahead_of()` on the hot path.** O(orders at level), and hot levels hold
    hundreds. `level.hpp` warns about it in its own comment. Use `shares_at()`, which is
    O(1) off the incrementally maintained `Level::shares`.

64. **Quoting `D` at 44.95% as the queue statistic.** It is a whole-book *message* count;
    queue advancement is in *shares at the touch while your order rests there*, and it
    omits the `U` delete legs (17.1 cancels per execution, not 15.6). **Fix:** publish the
    share-weighted at-our-level ratio the sim computes; the 17.1 is context.

65. **Naming the fourth model `exact`.** It is exact only with respect to displayed
    liquidity on one venue, and the name invites exactly the criticism it exists to
    preempt. It is `mbo`.

66. **Misdescribing what the generators cover.** Non-printable `C` *is* well covered (10%
    and 33%); what is missing is a `C` at the resting price, timestamp ties, any halt
    sequence, a `Q` at a tracked price, and an opposite-side add that locks a tracked
    price. Getting the coverage claim backwards means the wrong tests get written.

67. **A header-only project with non-inline definitions,** and a sketch that does not
    compile: two members named `fees_` in one class, an undeclared `lanes_`, a missing
    `operator==`. The build is `-Wall -Wextra -Wpedantic -Werror`; compile the sketch
    before circulating it. (`__int128` really is rejected under `-Wpedantic -Werror`, so
    the WAC-without-128-bit arithmetic stands.)

---

## 12. Order of work

1. **Prerequisites** (§8.1): `const` overloads, `Book::first_order`, `locked()` /
   `strictly_crossed()`, `apply_ex()`. Small, mechanical, and everything depends on them.
2. **`make_queue_feed.py`** with `--cancel-front-bias`, `--tie-burst`, the halt sequence,
   a `C` at the resting price, a `Q` at a tracked price, and a locking opposite-side add.
   Nothing below can be tested without it.
3. **`money.hpp` / `fees.hpp` + `test_fees.cpp`** with the goldens at MSFT's real price.
   Cheapest, and it catches trap 32 and trap 33 before they reach the headline.
4. **`event.hpp` + `queue_model.hpp`** — `resolve`, the two arithmetic rules, the clamp,
   the four models, R1/R2/R3 — plus `test_queue.cpp`.
5. **`python/reference/queue_sim.py` + `queue_differential.py`.** Byte-identical fill logs
   or the model is not trusted.
6. **`fuzz_queue.cpp`** — I1–I7, per event, on `make_queue_feed.py` output only.
7. **`latency_model.hpp` + `simulator` plumbing** — atomic timestamp groups, the shared
   session FIFO, `EventKey`, arrival-time queue snapshots — plus the zero-latency book
   equivalence test (**I11**) and the four cancel-race buckets.
8. **`ledger.hpp` + `markout.hpp`** with **I8** and **I9** wired in from the start.
9. **`strategy.hpp` + `strategies.hpp` + `test_sim.cpp`** — S1, S2, S2b, S3, S7, and the
   losing control.
10. **`taker.hpp`**, the flatten path, and **I14**.
11. **`report.hpp` + `tools/queue_backtest.cpp`**, provenance headers, goldens, CI.
12. **The delete-one-order oracle** (§9.3) on synthetic feeds. This is the phase's external
    oracle and it should land before any real-data run, not after.
13. **`tools/latency_sweep.cpp` + `python/analysis/*.py`**, the charts.
14. **`make_toxic_feed.py`** with its known answer, and the demonstration that markouts
    detect adverse selection when it is present.
15. **The real-data run**, if a licensed feed is re-obtained: the band, its coverage rate,
    where `mbo` sits, the share-weighted at-touch cancel ratio, the markouts, the `P`
    side census, and the intraday crossed check.
16. **`docs/phase6-results.md`** — mechanism and measurement in two separate tables, §10.3
    in the first paragraph.
