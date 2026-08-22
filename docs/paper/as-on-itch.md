# Avellaneda–Stoikov on NASDAQ TotalView-ITCH, with fill-model uncertainty bands

*Kareem Jandali · École de technologie supérieure*

> **Status: methodology complete, results pending data.** Every section below
> that describes *how* is finished. Every section that reports *what happened* is
> generated from committed artifacts by `scripts/paper-report.py`, and those
> artifacts do not exist yet — the evaluation needs at least three symbols across
> the liquidity spectrum and at least three trading days, with the calibration
> day excluded. The generator refuses to emit a results table it does not have,
> so this document cannot accidentally look finished. See §9.

---

## 1. Abstract

Avellaneda–Stoikov (2008) prices a market maker's quotes around a *reservation
price* displaced by inventory, and needs a fill-intensity curve
λ(δ) = A·e^(−kδ) to set its spread. Implementations almost universally **assume**
A and k. This paper measures them, from the author's own simulated fills, through
a queue-position-resolved order-book model built directly from NASDAQ
TotalView-ITCH 5.0 message data.

Two methodological claims carry the work. First, fill-model uncertainty is
reported as a **band over worlds** rather than a band over gradings of one world
— a distinction that is usually elided and that changes what the band means (§4).
Second, the intensity curve is measured **per fill model**, because λ̂ is
estimated *through* a queue model and is therefore conditional on it (§6).

The headline question is deliberately small: *does inventory-aware quoting lose
less than naive symmetric quoting, and through which mechanism* — fewer toxic
fills, or smaller inventory excursions? A three-arm design separates the effect
of the **spread choice** from the effect of the **inventory skew**, which a
two-arm comparison bundles together (§7).

## 2. Data and venue

NASDAQ TotalView-ITCH 5.0, the exchange's own binary message feed: every
displayed order add, execute, cancel, delete and replace, with nanosecond
timestamps, for every security on the venue. The reconstruction is from raw
messages — no vendor's book, no bar data, no snapshots.

What this feed **is not** bounds everything downstream, so it is stated here
rather than in the limitations:

- **Displayed liquidity only.** Hidden and midpoint orders never appear as
  resting size. They appear only when they trade, as non-attributed executions.
  A book reconstructed from this feed is the *displayed* book, and a strategy
  evaluated against it is competing with less than the real queue.
- **One venue.** US equities trade across more than a dozen. Order flow this
  paper never sees was routed elsewhere, and the NBBO is not reconstructible
  from NASDAQ alone.
- **No principal identity.** Orders are anonymous, so nothing here can
  distinguish informed from uninformed counterparties directly. Adverse
  selection is inferred from markouts, never observed.

Correctness of the reconstruction is established independently of this paper: a
C++ book and an independent Python reference implementation must produce
byte-identical snapshots over a full trading day, and CI enforces it.

## 3. Fill models

A backtest of a passive strategy has no ground truth about its own fills. The
order rested; whether it *would have* traded depends on where it sat in a queue
nobody recorded. This project carries four models rather than choosing one:

| model | assumption |
|---|---|
| **naive** | any trade at our price fills us |
| **optimistic** | we are at the front of the queue |
| **mbo** | queue position tracked from message-by-message data, with the set of references resting ahead of us at arrival |
| **pessimistic** | we are at the back of the queue |

`mbo` is the one that uses the information the feed actually contains; the other
three bracket it. The spread between them is not error to be minimised — it is
the honest width of what a passive backtest can claim.

## 4. The feedback wall, and what the band now means

This is the section the rest of the paper depends on, and it is where published
results are most often ambiguous.

**Phase 6's band is four gradings of one world.** A strategy that cannot see its
fills emits one intent stream; four fill models score that identical stream four
ways. The difference between lanes is attributable to the model alone — not by
argument but *structurally*, because no feedback path exists by which the
decisions could have differed.

**That construction is impossible for an inventory-aware strategy.** A-S's
reservation price is a function of inventory `q`, and inventory is a function of
fills. A strategy in the pessimistic lane gets fewer fills, carries different
inventory, quotes differently, and fills differently again. The lanes diverge
from the first fill onward, and there is no longer one intent stream to grade.

**So the band becomes four closed-loop runs — a band over worlds.** Each is a
complete, internally consistent answer to *what would this strategy have done if
fills worked like this*. It is wider than the phase-6 band for a reason that is
not noise: it now includes the strategy's own reaction to being filled
differently.

Both constructions are legitimate and they answer different questions. Phase 6
asks how much the fill model matters to the *score* of a fixed strategy; this
paper asks how much it matters to a strategy that *reacts*. Reporting one while
describing the other would be the single most misleading thing this work could
do, so the two live in separate code paths — `backtest.hpp` and
`closed_loop.hpp` — and produce differently-labelled output.

**Half the wall stays up.** The strategy is told its fills and its position. It
is *not* told its queue position: `SimFill` carries the shares resting ahead of
it at arrival, and the `FillEvent` handed to a strategy deliberately does not
copy that field. It is the estimate whose error bars are the entire subject of
§3, and a strategy conditioning on it would be conditioning on the thing being
measured. The omission is enforced by a compile-time assertion.

## 5. Strategy

Finite-horizon Avellaneda–Stoikov:

- reservation price **r(s, q, t) = s − q·γ·σ²·(T − t)**
- total spread **δᵃ + δᵇ = γ·σ²·(T − t) + (2/γ)·ln(1 + γ/k)**, split
  symmetrically about r

σ is estimated online from the realised variance of **mid changes**, not
returns: A-S models arithmetic Brownian motion, so the volatility it wants is in
dollars per root-second. Sampling is on a fixed time grid rather than per
message, because per-message sampling makes the estimate a function of how busy
the symbol is.

### 5.1 Units, and a trap

The paper's two terms are not dimensionally consistent unless inventory is
treated as a dimensionless count: the spread term requires γ in 1/price, the
reservation term in 1/(shares·price). Implementations resolve this silently and
differently, which is part of why published A-S results are hard to compare.
Here it is resolved explicitly — dollars and seconds throughout, γ in 1/dollar in
both terms, and inventory entering as `q / inventory_unit`, a named parameter
that is reported rather than buried.

The consequence of leaving it implicit is not subtle. Avellaneda and Stoikov's
worked example uses γ = 0.1, k = 1.5 in arbitrary units. The intensity term is
approximately `2/k` for small γ/k, so k = 1.5 **per dollar** implies a total
spread of about **170 ticks** on a book that quotes one wide. Ported unchanged to
a penny-spread equity, the strategy quotes all day and never fills. This
implementation did exactly that on its first run; a regression test now asserts
both that the corrected defaults are sane and that the paper's values are the
trap, so the fix cannot be silently reverted.

### 5.2 A known pathology, flagged rather than mitigated

As t → T the inventory term decays to zero, so the model stops skewing for
inventory precisely when it has least time left to unload it. This follows from
the finite-horizon formulation, which assumes terminal inventory is liquidated at
the mid — something no desk can do. A floor on (T − t) is available as a
mitigation and **defaults to off**, so the pathology is visible in the results
rather than papered over, and the strategy counts quotes placed in the last tenth
of the session while holding inventory. Guéant–Lehalle–Fernandez-Tapia's
inventory-bounded variant is the principled fix and is not implemented here.

## 6. Calibrating λ(δ)

λ̂(δ) = fills(δ) / exposure(δ), where exposure is integrated **per order** over
time: two orders resting one second at the same depth is two order-seconds,
because λ is the intensity for a single order.

Four things are excluded from the denominator, each of which would bias k in a
determinate direction: time when the symbol is not tradable, time when the mid is
unusable, hidden iceberg reserve (in no queue, and so not exposed), and orders
that are no longer live. Depth is **integrated, not assigned at placement** — the
mid moves while an order rests, so a single order migrates between depth buckets
during its life. Taker fills are excluded entirely: λ(δ) describes a resting
order being hit, and crossing the spread is a decision rather than an arrival.

The fit is log-linear and **Poisson-weighted** — a bucket's fill count is a
count, so var(ln λ̂) ≈ 1/fills. Buckets with exposure but zero fills cannot be
logged, are excluded, and are **counted**: dropping them silently flattens the
curve, because the deep buckets are exactly the ones that fail to fill.

### 6.1 The conditioning decision

λ̂ is measured *through* a queue model, so A and k are properties of (this feed,
this strategy, **that model**). Calibrating once under one model and evaluating
four would leave three lanes using a fill curve fitted in a world they do not
inhabit — reintroducing precisely the cross-contamination the closed-loop design
removes.

**This work calibrates per lane.** Four passes, four curves, and the artifact
records `calibrated_per_lane` so a reader never has to infer it. The cost is
stated: four passes instead of one, and a band that now carries variation from
two sources — different fills *and* different fitted parameters — which is the
price of each world being internally consistent.

### 6.2 The touch misfit

A-S assumes fill intensity depends only on depth. At δ = 0 it depends mostly on
**queue position**, which the exponential has no way to express, so the touch
bucket is expected to sit below the fitted curve. This is a known limitation of
the model and the residual figure is the evidence for it rather than an assertion
about it.

### 6.3 What the fit came out as

<!-- generated:calibration:begin -->

> **Not measured.** `validation/intensity.json` is not committed, so no fitted A or k appears here. The calibration runs with:
>
> ```
> build/calibrate_intensity data/sliced/SYM-DAY.gz \
>     --json validation/intensity.json
> ```
>
> Until it does, §5's spread formula is being fed the **default** k rather than a measured one — the paper says so rather than printing a number it does not have, and §6.1's per-lane decision has nothing to be per-lane about yet.

<!-- generated:calibration:end -->

## 7. Experimental design

**Three arms**, because two cannot answer the question:

| arm | what it is |
|---|---|
| `symmetric-touch` | quote the touch, both sides, ignore inventory — the naive baseline |
| `as-gamma0` | A-S with the inventory skew **off**: same spread formula, same re-quote discipline, same size |
| `as` | A-S proper, at each swept γ |

The middle arm is the control. A-S differs from a touch-maker in inventory
awareness *and* in where it quotes *and* in how often it re-quotes; an
improvement over the touch-maker could come from any of the three. Comparing
against γ = 0 holds everything fixed except the skew, so **"A-S beats a naive
maker"** and **"the skew is what beat it"** remain separable claims. They are
routinely reported as one.

γ = 0 is handled by taking the limit — `(2/γ)·ln(1 + γ/k) → 2/k` — rather than
refusing the input, so the control stays inside the same code path as the
treatment.

Every headline number is a band across the four fill models. γ is **swept and
plotted**, never chosen. Results are reported **per symbol-day and never pooled**:
with a handful of symbol-days there is no significance to claim, and an average
invites exactly the claim the data cannot support. Calibration and evaluation
**never share a day** — the driver exits non-zero rather than warning if they do.

<!-- generated:results:begin -->

> **No results.** `validation/as-experiment.json` is not committed, so this
> section is empty *by construction*, not by omission. `scripts/paper-report.py`
> emits a results table only from a committed artifact, and there is no path by
> which a number reaches this page without one.
>
> The artifact needs, before it can back this section:
>
> - **≥ 3 symbols** across the liquidity spectrum and **≥ 3 evaluation days**
> - the **calibration day excluded** from evaluation — the harness exits
>   non-zero rather than warning if they overlap
> - four closed-loop lanes per arm per symbol-day (§4: a band over *worlds*)
>
> ```
> bench/as-experiment.py --build build --out validation/as-experiment.json \
>     --feed SYM:YYYY-MM-DD:data/sliced/SYM-DAY.gz [--feed ...] \
>     --calibration-day YYYY-MM-DD --k <the measured k, not the assumed one>
> ```
>
> **The seven predictions in `docs/build-plan-9-12.md` §11.3 are ungraded.**
> They were committed before the harness was written, and they will be graded
> here — kept or falsified — by computation against the bars in this script,
> not by anyone's reading of the table.

<!-- generated:results:end -->

## 8. Limitations

Stated at length because the conclusion depends on them.

- **Displayed liquidity only.** No hidden or midpoint size. A maker competing
  against the real queue faces more competition than this models.
- **One venue.** No NBBO, no routing, no cross-venue adverse selection.
- **No market impact.** Our orders consume liquidity that historical
  participants never saw, and those participants never react to us. This is a
  counterfactual, not a replay.
- **Small N.** A handful of symbol-days. The day-level spread is reported
  instead of a mean, and no significance is claimed anywhere.
- **λ̂ is conditional on a queue model** (§6.1), and the queue model is itself
  the estimate under study in §3.
- **Latencies are synthetic.** The latency sweep applies a modelled delay; it is
  not a measurement of a real path to a real exchange.
- **The strategy is evaluated, not deployed.** Nothing here has traded.

## 9. What remains

The machinery is complete and tested; the evaluation is not run. Specifically:

1. **Data.** At least three symbols across the liquidity spectrum (a large-cap,
   a mid-cap, an ETF) and at least three trading days, with the calibration day
   excluded from evaluation.
2. **The outside reader.** The plan requires §4 to be reviewed by someone who
   did not write it. That has not happened, and it is the section most likely to
   be wrong in a way its author cannot see.
3. **Latency sensitivity**, via the existing sweep, against the pre-registered
   prediction that A-S degrades faster than the baseline because it re-quotes
   more.

Predictions for all of the above were committed **before** the harness was
written; they are in `docs/build-plan-9-12.md` §11.3 and will be graded in print
whether or not they survive.

## 10. Reproducing

```
scripts/paper-figures.sh          # every figure, from committed JSON
python3 scripts/paper-report.py   # every results table, from the same
python3 scripts/paper-html.py     # the page, from this Markdown
scripts/paper-pdf.sh              # the PDF, from that page
```

CI runs the first three with `--check`, so no number and no chart in this
document can drift from the artifact that produced it. Three properties are
enforced rather than intended:

- **The results tables are generated, never typed.** The block in §7 is written
  by `paper-report.py` between markers, and the seven verdicts in §7.5 are
  *computed* against bars transcribed once from the plan. An earlier report
  script in this repository printed "kept" from a string literal after the
  numbers had moved outside the predicted range; that is the failure this rules
  out by construction.
- **A figure whose artifact is missing is an error, not a skip.** If a chart is
  committed and the JSON behind it is not, `paper-figures.sh` fails. Provenance
  — artifact path, its SHA-256, the exact command — lives in
  `docs/figures/paper/manifest.json` next to the figures.
- **The generator is exercised with data, in CI, on every push.** The
  no-artifact path is what runs today, so CI also builds synthetic feeds, runs
  the full experiment through them, and asserts that the paper comes back with a
  graded table. A results generator first executed on the day the real data
  arrives is a results generator nobody has tested.

The PDF is a build output and is not committed: its bytes depend on a font
cache, and a diff cannot referee that. Every *input* to it is committed and
checked.
