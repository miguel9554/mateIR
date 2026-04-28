# SyncType propagation and CDC checking

## Core rule

CDC correctness should be checked at flop inputs, not at arbitrary module port
connections.

For a flop clocked by domain `D`, the value driving its `.d` input must be
synchronous to domain `D`.

```text
flop.clock_domain == D
flop.d SyncType must be SyncSignal{clock_domain = D, ...}
```

Anything else is a CDC violation unless there is an explicit synchronizer model
or annotation that changes the value's `SyncType` before it reaches the flop.

## Port connections are dataflow, not CDC checks

A module port connection should propagate `SyncType`; it should not by itself be
treated as illegal.

Examples:

```text
async parent signal -> child input
```

This is a valid connection. The child input becomes async unless a synchronizer
or explicit annotation transforms it.

```text
sync(D1) parent signal -> child input
```

This is also a valid connection. The child input receives `SyncSignal{D1}`.

If that child input later feeds a flop clocked by `D2`, the flop-input check
decides whether the design is valid:

```text
D1 == D2  => ok
D1 != D2  => CDC violation, unless synchronized
async     => CDC violation, unless synchronized
```

## Domain propagation rules

Each signal/DFG node has a `SyncType`:

```cpp
SyncSignal { ClockId clock_domain, ResetDomains reset_domains }
ClockSignal { ClockId clock_domain }
ResetSignal { ResetId reset_domain }
AsyncSignal {}
```

Propagation through ordinary combinational dataflow:

- all synchronous contributors with the same `ClockId` produce `SyncSignal`
- reset-domain sets are unioned while the result remains synchronous
- synchronous contributors with different `ClockId`s produce `AsyncSignal`
- any async contributor produces `AsyncSignal`
- clock and reset signals used as data produce `AsyncSignal`
- when a result becomes `AsyncSignal`, reset-domain information is discarded

Constants do not force a domain by themselves. A constant-only expression can
remain unclassified until it is combined with domain-bearing data or assigned to
a context that gives it meaning.

## Boundary seeds

Top-level YAML seeds initial `SyncType` for external inputs:

- top clocks become `ClockSignal`
- top resets become `ResetSignal`
- top synchronous inputs become `SyncSignal`
- top async inputs become `AsyncSignal`

Flop Q outputs seed synchronous data:

```text
flop.q => SyncSignal{flop.clock_domain, flop.reset_domains}
```

After these seeds are placed, normal dataflow propagation should derive the
remaining signal and port `SyncType`s.

## Synchronizers

Synchronizers are the explicit mechanism that changes an async or cross-domain
value into a synchronous value for a target domain.

Until there is a richer structural synchronizer model, `synchronized_into` is an
intent annotation. Semantically, it allows a value that would otherwise be async
or in another domain to be treated as synchronous after the synchronizer point:

```text
input SyncType:  AsyncSignal or SyncSignal{D1}
annotation:      synchronized_into D2
after sync:      SyncSignal{D2}
```

This annotation should be applied at the point where the design intentionally
synchronizes the value, not as a blanket permission for arbitrary downstream
CDC.

## What not to check

Do not reject module port connections merely because parent and child declared
domains differ.

That style of check is too early and too local. A port connection is just an
edge in the dataflow graph. The correct question is what `SyncType` eventually
reaches each flop `.d`.

In particular:

- `AsyncSignal -> child input` should propagate async
- `SyncSignal{D1} -> child input` should propagate `D1`
- if the child later uses the value in domain `D2`, the flop-input check catches
  the violation
- if the child synchronizes the value into `D2`, propagation should reflect that
  and the flop-input check should pass

## CDC validation algorithm

For every flop:

1. Determine the `ClockId` of the flop.
2. Determine the propagated `SyncType` of every leaf driving the flop `.d`.
3. Merge multi-leaf or expression contributors using the normal propagation
   rules.
4. Accept only `SyncSignal` with the same `ClockId` as the flop.
5. Reject `AsyncSignal`, `ClockSignal`, `ResetSignal`, or `SyncSignal` with a
   different `ClockId`.

This makes the validation rule simple:

```text
Only sync(D) may drive flop(D). Everything else must be synchronized first.
```

## Implication for current code

The current `validateCrossModuleConnections` logic should be reconsidered. It
can reject valid dataflow too early, and it can encode special cases that belong
in propagation or flop-input validation.

The long-term direction should be:

- use hierarchy connections to propagate `SyncType` across module boundaries
- do CDC validation at flop `.d` inputs
- keep synchronizer annotations as transformations in the propagated model

