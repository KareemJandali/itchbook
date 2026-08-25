#!/usr/bin/env python3
"""rate-sweep.py — the rate-latency curve, and the two numbers on it.

Offers the same feed at a ladder of rates and records what comes out the other
end. Two annotations are the deliverable and everything else is supporting
evidence:

  * **The knee.** The rate at which queueing delay appears -- where p99 leaves
    the flat region and starts climbing. Below it the pipeline is idle between
    messages; above it there is a queue, and the latency you measure is mostly
    time spent waiting in it.
  * **The max sustainable rate.** The highest rate with ZERO ring-full drops and
    ZERO kernel drops. Both, because on loopback the socket buffer overflows
    before the ring does, and a figure that counted only ring drops would be a
    claim about a pipeline that was quietly losing datagrams upstream of it.

ONE TIMES REAL TIME IS COMPUTED, NOT QUOTED. The ladder is anchored to the
feed's own clock via itch_census --timing, which reports the span between the
first and last timestamp in the file. A real-time figure typed into a script is
a constant that drifts from the feed it claims to describe.

BEST OF N, NOT THE MEDIAN. Latency noise here is one-sided: a descheduled thread
or a competing process can only make a run slower, never faster. So the best run
at each rate is the one that measured the pipeline rather than the machine's
other tenants -- the same reasoning phase 9.9 used after one outlier put the
"within-variant spread" of a reproducible 1.9x effect at 190%.

THE SENDER IS A RESULT TOO. Every rate records the generator's own lateness, and
a rate where the sender could not keep its schedule is marked. Past that point
the experiment measured the load generator, and a curve that does not say so is
a curve about the wrong program.

Usage:
  bench/rate-sweep.py --build build --out out/rate-sweep.json
                      [--feed f.gz] [--messages N] [--repeats N]
                      [--multipliers 1,2,5,10,...] [--ring-log2 N]
"""
import argparse
import json
import os
import re
import select
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The sender's qualification bar, in nanoseconds at p99.9. Mirrors
# kSenderTailBudgetNs in tools/mold_replay_udp.cpp; it was written out twice as
# a literal 10000 and the two copies are one edit away from disagreeing about
# what a qualified run is.
SENDER_BUDGET_NS = 10000


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def wait_ready(proc, timeout=30.0):
    """Block until the receiver says its BOOK THREAD exists.

    Binding the port -- which is what this used to wait for -- happens 50 ms of
    clock calibration and an 8.4M-entry allocation short of a consumer:
    measured on the machine this was written on, the gap between the port
    appearing in /proc/net/udp and READY was 91-102 ms across six runs. The
    receiver thread stamps arrivals through all of it while nothing drains the
    ring, so every message the generator sent into that window carried the
    remaining stall as its wire-to-book sample, and multi-millisecond samples
    turned up at every rate on the ladder including 1x.

    A sleep would only be a guess at somebody else's allocator, so the receiver
    is asked to say so instead.

    THE DEADLINE IS ON THE PIPE, NOT ON THE LOOP. The first version tested
    time.time() at the top of a loop whose body blocked in readline(), so a
    receiver that neither printed nor exited -- wedged in the ref-table
    allocation, or pinned to a core that does not exist -- hung the whole sweep
    instead of failing one run. select() puts the timeout where the blocking is.
    """
    deadline = time.time() + timeout
    while True:
        remaining = deadline - time.time()
        if remaining <= 0:
            print("    receiver never reported READY", file=sys.stderr)
            proc.kill()
            return False
        if not select.select([proc.stdout], [], [], remaining)[0]:
            continue
        line = proc.stdout.readline()
        if line == "":
            return False                      # receiver died before it was ready
        if line.startswith("READY"):
            return True
        print(f"    receiver: {line.rstrip()}", file=sys.stderr)


def real_time_rate(build, feed, work):
    """One times real time, from the feed's own timestamps."""
    out = os.path.join(work, "timing.json")
    r = run([os.path.join(build, "itch_census"), feed, "--timing", out])
    if r.returncode != 0:
        sys.exit(f"itch_census failed:\n{r.stderr}")
    t = json.load(open(out))
    if not t.get("real_time_msg_per_s"):
        sys.exit("the feed carries no usable timestamps; cannot anchor the ladder")
    return t


def one_run(build, packets, rate, port, ring_log2, work, expect, cpus, rt_priority=0):
    """One offered rate. Returns the merged sender and receiver record.

    `cpus` is (recv, book, sender) or (None, None, None). Pinning matters more
    here than anywhere else in this repository: phase 4 measured 19.3%
    run-to-run variance on a single-threaded benchmark without it, and this has
    three threads competing across two processes. An unpinned sweep measures the
    scheduler, which is what every number in docs/phase10-results.md currently
    admits to.

    The two pipeline threads pin themselves -- wire_to_book takes --cpu-recv and
    --cpu-book -- but the SENDER has no such flag, and it is the process whose
    lateness decides whether a run counts at all. taskset puts it on its own
    core from the outside, which is the same mechanism and one fewer flag to
    keep in sync.
    """
    cpu_recv, cpu_book, cpu_send = cpus
    # PER-RUN PATHS. These were three fixed names reused by every run of a
    # sweep -- 55 of them at the default ladder and repeat count. The hazard is
    # not a truncated file (read_buckets catches OSError and StopIteration, and
    # a half-written final record raises ValueError loudly): it is a COMPLETE
    # hist.csv left by an earlier run, which is indistinguishable from this
    # run's and is read with no error at all. The port is already unique per
    # run; borrow it.
    rj = os.path.join(work, f"recv-{port}.json")
    sj = os.path.join(work, f"send-{port}.json")
    hist = os.path.join(work, f"hist-{port}.csv")
    recv_cmd = [os.path.join(build, "wire_to_book"), "--port", str(port),
                "--ring-log2", str(ring_log2), "--timeout-ms", "5000",
                "--rcvbuf-mb", "16", "--expect-messages", str(expect),
                "--json", rj, "--hist-csv", hist, "--quiet"]
    if cpu_recv is not None:
        recv_cmd += ["--cpu-recv", str(cpu_recv), "--cpu-book", str(cpu_book)]
    recv = subprocess.Popen(recv_cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    if not wait_ready(recv):
        recv.kill()
        recv.wait()
        return None
    send_cmd = [os.path.join(build, "mold_replay_udp"), packets, "--host", "127.0.0.1",
                "--port", str(port), "--rate", str(int(rate)), "--json", sj, "--quiet"]
    if rt_priority:
        send_cmd += ["--rt-priority", str(rt_priority)]
    if cpu_send is not None:
        send_cmd = ["taskset", "-c", str(cpu_send)] + send_cmd
    send = run(send_cmd)
    tail = recv.communicate()[0]
    rc = recv.returncode
    if rc not in (0, 3, 4) or not os.path.exists(rj):
        print(f"    receiver exited {rc}\n{tail}", file=sys.stderr)
        return None
    if send.returncode != 0:
        print(f"    sender exited {send.returncode}\n{send.stderr}", file=sys.stderr)
        return None
    r = json.load(open(rj))
    s = json.load(open(sj))
    r["exit"] = rc
    r["sender"] = s
    r["buckets"] = list(read_buckets(hist))
    return r


def read_buckets(path):
    try:
        with open(path) as f:
            next(f)
            for line in f:
                lo, hi, n = line.strip().split(",")
                yield [int(lo), int(hi), int(n)]
    except (OSError, StopIteration):
        return


def best_of(runs):
    """Lowest p99 wins -- but only among runs that agree on whether they lost.

    A lossy run and a clean run at the same rate are not two samples of one
    thing. Taking the best across that boundary would let a run that dropped
    half the feed -- and therefore did almost no work -- report the best
    latency at its rate, which is how a cliff gets smoothed into a slope.
    """
    clean = [r for r in runs if r["ring_full_drops"] == 0 and r["staging_overflow"] == 0
             and r["kernel_drops"] in (0, None)]
    pool = clean if clean else runs
    # AND THE SENDER GETS ITS OWN CRITERION.
    #
    # Selecting one whole run by lowest receiver p99 and carrying its sender
    # block along is a lottery for the generator: the two criteria measure
    # different processes. Worse than a lottery, in fact -- the selection is
    # biased TOWARD the stalled repeat, because a sender parked for 47 ms sends
    # nothing while it is parked, and the messages it does not send are
    # messages the receiver does not queue. The stalled run can therefore win
    # on receiver p99 and put its own 47 ms into the sender column.
    #
    # So prefer runs whose generator kept its schedule, and record what the
    # other repeats' senders did rather than discarding them.
    ontime_pool = [r for r in pool
                   if r["sender"]["lateness_ns"]["p999"] <= SENDER_BUDGET_NS]
    best = min(ontime_pool if ontime_pool else pool,
               key=lambda r: r["wire_to_book_ns"]["p99"])
    best["runs_at_this_rate"] = len(runs)
    best["clean_runs_at_this_rate"] = len(clean)
    # OVER ALL RUNS, not over `pool`. These four numbers sit next to each other
    # in the artifact and a reader will form ratios out of them, so they have to
    # share a denominator; the pool-restricted list is only for the min() above.
    #
    # AND NOTE WHAT THIS MAKES THE QUALIFICATION MEAN. Preferring an on-time
    # repeat is best-of-N for the sender: a rate now qualifies if ANY clean
    # repeat's generator held its schedule, where before it depended on which
    # repeat happened to win on receiver p99 -- a lottery that was biased
    # toward the stalled one, since a sender parked for tens of milliseconds
    # sends nothing while parked and so leaves the receiver less to queue.
    # Best-of-N is the right rule for quoting a latency, because the run being
    # quoted is the one whose generator worked. It is a LOOSER rule than the
    # lottery it replaces, which is why the denominator is published beside it
    # rather than left for the reader to assume.
    best["sender_ontime_runs_at_this_rate"] = sum(
        1 for r in runs if r["sender"]["lateness_ns"]["p999"] <= SENDER_BUDGET_NS)
    best["sender_p999_all_runs"] = sorted(
        r["sender"]["lateness_ns"]["p999"] for r in runs)
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default="build")
    ap.add_argument("--out", default="out/rate-sweep.json")
    ap.add_argument("--feed", help="an existing .gz feed; generated if omitted")
    ap.add_argument("--messages", type=int, default=200000)
    ap.add_argument("--session-seconds", type=float, default=60.0)
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--ring-log2", type=int, default=16)
    ap.add_argument("--port", type=int, default=27100)
    ap.add_argument("--cpu-recv", type=int, help="pin the receiver thread to this core")
    ap.add_argument("--cpu-book", type=int, help="pin the book thread to this core")
    ap.add_argument("--cpu-sender", type=int,
                    help="run the load generator on this core, via taskset")
    ap.add_argument("--rt-priority", type=int, default=0,
                    help="ask for SCHED_FIFO at this priority in the load generator. "
                         "Its pacing is accurate to tens of nanoseconds at p50; the "
                         "lateness that disqualifies a sweep is a tail where the thread "
                         "is not running. Needs CAP_SYS_NICE -- root, or once: "
                         "sudo setcap cap_sys_nice=ep <build>/mold_replay_udp. Denial is "
                         "reported, never silent.")
    ap.add_argument("--allow-unqualified", action="store_true",
                    help="exit 0 even when no rate on the ladder had a sender that "
                         "kept its 10 us schedule. For a shape test -- CI checks that "
                         "the sweep runs end to end and produces the columns the "
                         "report needs, on a runner that could never qualify. A "
                         "measurement run must NOT pass this: the non-zero exit is "
                         "what keeps an unqualified sweep from being treated as a "
                         "result by anything that only checks the status code. "
                         "(It is not the last line of defence -- the artifact "
                         "records sender_qualified_rates, and phase10-report.py "
                         "stamps the document itself -- but it is the only one a "
                         "shell script sees.)")
    ap.add_argument("--multipliers", default="1,2,5,10,25,50,100,200,400,800",
                    help="offered rate as multiples of one times real time")
    ap.add_argument("--extend", type=int, default=6,
                    help="if the top rung is still clean, double this many more "
                         "times looking for the cliff; 0 to take the ladder as given")
    a = ap.parse_args()

    build = os.path.abspath(a.build)
    for t in ("wire_to_book", "mold_replay_udp", "mold_wrap", "itch_census"):
        if not os.path.exists(os.path.join(build, t)):
            sys.exit(f"missing {build}/{t}")

    work = tempfile.mkdtemp(prefix="rate-sweep-")
    try:
        feed = a.feed
        if feed is None:
            feed = os.path.join(work, "feed.gz")
            r = run([sys.executable, os.path.join(HERE, "scripts", "make-synthetic-feed.py"),
                     feed, "--messages", str(a.messages),
                     "--session-seconds", str(a.session_seconds)])
            if r.returncode != 0:
                sys.exit(r.stderr)
            print(r.stderr.strip(), file=sys.stderr)

        timing = real_time_rate(build, feed, work)
        base = timing["real_time_msg_per_s"]
        n_msgs = timing["messages"]
        print(f"feed: {n_msgs:,} messages over "
              f"{(timing['last_timestamp_ns'] - timing['first_timestamp_ns']) / 1e9:.1f}s "
              f"of session = {base:,.0f} msg/s at 1x real time")

        packets = os.path.join(work, "feed.pkt.gz")
        if run([os.path.join(build, "mold_wrap"), feed, packets]).returncode != 0:
            sys.exit("mold_wrap failed")

        # All three or none: pinning two of the three threads and letting the
        # rest float is worse than not pinning at all, because it looks pinned
        # in the output while the unpinned one still decides the tail.
        pins = (a.cpu_recv, a.cpu_book, a.cpu_sender)
        if any(p is not None for p in pins) and any(p is None for p in pins):
            sys.exit("--cpu-recv, --cpu-book and --cpu-sender must be given "
                     "together or not at all")
        if pins[0] is not None and len(set(pins)) != 3:
            sys.exit("the three cores must be distinct; sharing one is not pinning")
        cpus = pins

        mults = [float(x) for x in a.multipliers.split(",")]
        expect = int(n_msgs * 1.5)
        rows = []
        port = a.port
        extended = 0
        print()
        print("SENDER LATE p99.9 IS THE COLUMN THAT DECIDES THE VERDICT, and it is")
        print("shown because 'sender late' alone hides whether you are 2x over the")
        print("10 us bar or 200x. The measured pacing is accurate to tens of")
        print("nanoseconds at p50; the whole error budget is in this tail.")
        print(f"\n{'offered':>12} {'achieved':>12} {'x real':>7} {'p50 ns':>10} "
              f"{'p99 ns':>11} {'p99.9 ns':>11} {'late p99.9':>11} {'ring':>7} "
              f"{'kern':>6} {'occ':>7}  verdict")
        queue = list(mults)
        while queue:
            m = queue.pop(0)
            rate = base * m
            runs = []
            for _ in range(a.repeats):
                port += 1
                got = one_run(build, packets, rate, port, a.ring_log2, work, expect,
                              cpus, rt_priority=a.rt_priority)
                if got is not None:
                    runs.append(got)
            if not runs:
                print(f"{rate:12,.0f} {m:8g}   (no usable run)")
                continue
            row = best_of(runs)
            row["multiplier"] = m
            row["offered_rate"] = rate
            k = row["kernel_drops"]
            lossy = bool(row["ring_full_drops"] or row["staging_overflow"] or (k or 0))
            # The sender's own qualification test, from the methodology doc: a
            # generator whose p99.9 lateness is the same order as the latency
            # under measurement has invalidated the run, and the only way to
            # know is that it measures itself.
            row["sender_late"] = (row["sender"]["lateness_ns"]["p999"]
                                  > SENDER_BUDGET_NS)
            row["lossy"] = lossy
            # Offered is what we asked for; achieved is what the wire carried.
            # The pipeline can only have absorbed the second one, so that is
            # the number a sustained-rate claim rests on. They diverge exactly
            # when the sender stops keeping its schedule, which is why the
            # shortfall is recorded next to the lateness that causes it.
            row["achieved_rate"] = row["sender"].get("achieved_msg_per_s", 0.0)
            row["achieved_fraction"] = (row["achieved_rate"] / rate) if rate else 0.0
            rows.append(row)
            verdict = "LOSSY" if lossy else ("sender late" if row["sender_late"] else "clean")
            if row["achieved_fraction"] < 0.9:
                verdict += f" (sent {row['achieved_fraction'] * 100:.0f}% of offered)"
            late999 = row["sender"].get("lateness_ns", {}).get("p999", 0)
            print(f"{rate:12,.0f} {row['achieved_rate']:12,.0f} {m:7g} "
                  f"{row['wire_to_book_ns']['p50']:10,.0f} "
                  f"{row['wire_to_book_ns']['p99']:11,.0f} "
                  f"{row['wire_to_book_ns']['p999']:11,.0f} "
                  f"{late999:11,.0f} "
                  f"{row['ring_full_drops']:7,} {('?' if k is None else f'{k:,}'):>6} "
                  f"{row['peak_ring_occupancy']:7,}  {verdict}")

            # If the ladder's top rung is still clean, the sweep has not found
            # the cliff and "max sustainable" would be an artifact of where the
            # ladder happened to stop. Keep doubling until something drops, or
            # until the extension budget runs out -- and if it runs out, say so
            # rather than reporting the last rung as a measurement.
            if not queue and not lossy and extended < a.extend:
                extended += 1
                queue.append(m * 2)

        if not rows:
            sys.exit("no rate produced a usable run")

        # ---- the two annotations ------------------------------------------
        #
        # Max sustainable: the highest rate with nothing dropped anywhere, and
        # only counting rates below the first one that dropped. A rate that came
        # back clean ABOVE a rate that dropped is not evidence of headroom --
        # it is evidence the machine was busy during the other run.
        sustainable = None
        cliff = None
        for row in rows:
            k = row["kernel_drops"]
            if row["lossy"] or k is None:
                cliff = row
                break
            sustainable = row
        # Reached the top of the ladder with nothing dropped: the number below
        # is a LOWER BOUND on the sustainable rate, not the sustainable rate.
        # Reporting it as the latter would make the headline figure a fact
        # about how high this script was told to count.
        cliff_reached = cliff is not None

        # The knee: the first rate whose p99 exceeds twice the median p99 of the
        # flat region. Twice, because run-to-run noise on an unpinned box is
        # tens of percent and a threshold inside the noise finds a knee in a
        # straight line. The flat region is the first third of the ladder, or
        # the first three rungs, whichever is more.
        p99 = [r["wire_to_book_ns"]["p99"] for r in rows]
        flat_n = max(3, len(rows) // 3)
        flat = sorted(p99[:flat_n])
        baseline = flat[len(flat) // 2]
        knee = None
        for row, v in zip(rows, p99):
            if v > 2.0 * baseline:
                knee = row
                break

        qualified = [r for r in rows if not r["sender_late"]]
        out = {
            "cliff_reached": cliff_reached,
            "sender_qualified_rates": len(qualified),
            "rates_tried": len(rows),
            "feed": {"messages": n_msgs,
                     "session_seconds": (timing["last_timestamp_ns"]
                                         - timing["first_timestamp_ns"]) / 1e9,
                     "real_time_msg_per_s": base},
            # The per-row "buckets" are RAW TSC CYCLES, as histogram.hpp
            # recorded them, while every *_ns field beside them is nanoseconds.
            # Read naively they overstate by cycles_per_ns -- 3.6x on the box
            # this was written on -- and appear to hold samples above the max
            # reported two keys away. python/analysis/rate_latency.py knows;
            # nothing in the file said so.
            "buckets_unit": "tsc_cycles",
            "ring_slots": rows[0]["ring_slots"],
            "pinned": rows[0]["pinned_receiver"] and rows[0]["pinned_book"],
            "cpus": {"receiver": cpus[0], "book": cpus[1], "sender": cpus[2]},
            "clock": rows[0]["clock"],
            "repeats": a.repeats,
            "baseline_p99_ns": baseline,
            "knee": None if knee is None else
                    {"offered_rate": knee["offered_rate"], "multiplier": knee["multiplier"],
                     "achieved_rate": knee["achieved_rate"],
                     "p99_ns": knee["wire_to_book_ns"]["p99"]},
            "cliff": None if cliff is None else
                    {"offered_rate": cliff["offered_rate"], "multiplier": cliff["multiplier"],
                     "ring_full_drops": cliff["ring_full_drops"],
                     "kernel_drops": cliff["kernel_drops"],
                     "staging_overflow": cliff["staging_overflow"]},
            "max_sustainable": None if sustainable is None else
                    {"is_lower_bound": not cliff_reached,
                     "achieved_rate": sustainable["achieved_rate"],
                     "achieved_fraction": sustainable["achieved_fraction"],
                     "offered_rate": sustainable["offered_rate"],
                     "multiplier": sustainable["multiplier"],
                     "p50_ns": sustainable["wire_to_book_ns"]["p50"],
                     "p99_ns": sustainable["wire_to_book_ns"]["p99"],
                     "p999_ns": sustainable["wire_to_book_ns"]["p999"],
                     "buckets": sustainable["buckets"]},
            "rows": rows,
        }
        os.makedirs(os.path.dirname(os.path.abspath(a.out)) or ".", exist_ok=True)
        with open(a.out, "w") as f:
            json.dump(out, f, indent=1)

        print()
        if sustainable is None:
            print("max sustainable rate: NONE — the lowest rate on the ladder already "
                  "dropped. Lower the ladder or raise the ring.")
        else:
            bound = "" if cliff_reached else ">= "
            print(f"max sustainable rate: {bound}{sustainable['achieved_rate']:,.0f} msg/s "
                  f"achieved ({sustainable['offered_rate']:,.0f} offered, "
                  f"{sustainable['multiplier']:g}x real time), "
                  f"p50 {sustainable['wire_to_book_ns']['p50']:,.0f} ns / "
                  f"p99.9 {sustainable['wire_to_book_ns']['p999']:,.0f} ns")
            if not cliff_reached:
                print("  ...which is a LOWER BOUND, not a measurement: the top of the "
                      "ladder\n  was still clean after "
                      f"{extended} extension(s). Raise --extend or --multipliers.")
        if cliff is not None:
            print(f"cliff: {cliff['offered_rate']:,.0f} msg/s ({cliff['multiplier']:g}x) — "
                  f"{cliff['ring_full_drops']:,} ring-full, "
                  f"{cliff['kernel_drops']} kernel, "
                  f"{cliff['staging_overflow']:,} mid-block")
        if knee is None:
            print("knee: not reached — p99 never doubled over the ladder's flat region.")
        else:
            print(f"knee: {knee['offered_rate']:,.0f} msg/s ({knee['multiplier']:g}x), "
                  f"p99 {knee['wire_to_book_ns']['p99']:,.0f} ns "
                  f"vs {baseline:,.0f} ns baseline")
        if not qualified:
            print("\nNO RATE QUALIFIED. The sender's p99.9 lateness exceeded 10 us at "
                  "every rate\non the ladder, which is the same order as the latency "
                  "being measured. This\nsweep describes the load generator. Numbers "
                  "from it are not results.")
        elif len(qualified) < len(rows):
            print(f"\n{len(rows) - len(qualified)} of {len(rows)} rates had the sender "
                  "missing its schedule by more than 10 us\nat p99.9; those rows measure "
                  "the generator as much as the pipeline.")
        if not out["pinned"]:
            print("\nUNPINNED. These describe a scheduler as much as a pipeline; "
                  "phase 4 measured 19.3% run-to-run variance without pinning.")
        print(f"\nwrote {a.out}")
        # AND THE EXIT CODE SAYS SO. Every refusal above this line was a
        # print: the sweep announced that its numbers were not results and
        # then returned 0, so pinned-run.sh carried on, the report scripts
        # emitted a headline, and nothing downstream could tell a qualified
        # sweep from a disqualified one without re-reading the prose.
        if (not qualified or not out["pinned"]) and not a.allow_unqualified:
            return 3
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main() or 0)
