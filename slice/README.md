# The slice: the asynchronous calling convention end to end (first increment, 2026-11-28)

One thin end-to-end slice of the asynchronous calling convention, built on
one `operation_handle` strategy: typed handles, with VARIANT's `choice` as
the merge and TYPED one build switch away. It puts together what earlier
experiments measured one question at a time:

- `start` returns a handle, and the handle protocol is `typed-handle/`'s,
  copied unchanged as this directory's first commit;
- a stand-in `function` that keeps P4223's constructor and hides the
  entry, generating it from the factory's type (as `kv-handle/`'s `hfn`
  did);
- frames on an async stack, a basic library implementation offered as the
  frame allocator, each callee's frame released when it completes;
- results constructed where the caller wants them, across the erasure, in
  four designs (W-163, the deciding experiment for `kv-dest/`'s 3A, 4A
  and 5A, folded into the slice);
- kv_store's spine as bodies, each hand-lowered as a compiler would lower
  its direct-style source.

Everything under "Observed" was run or measured. "Reading" is inference.

## Files

| File | What |
|---|---|
| `handle.hpp`, `payload.hpp`, `env.hpp`, `algos.hpp`, `sched.hpp` | `typed-handle/`'s protocol and algorithms, unchanged |
| `stack.hpp` | The async stack: a strand of slabs with `push` and `pop`, and `get_frame_allocator` |
| `function.hpp` | The stand-in `function`, its generated entry points, the continuation record and the four destination designs; `erase_handles` |
| `kv.hpp` | The bodies, the interfaces they're behind, the root |
| `stores.cpp` | The definitions behind the interfaces (the factories), compiled on their own |
| `sync.hpp` | The synchronous yardstick: the same bodies as plain virtual functions |
| `scenarios.hpp` | The workloads, as one X-macro shared by the checks and the bench |
| `checks.cpp` | Every scenario's outcome, copies and moves, frame storage and heap allocations |
| `bench.cpp`, `cg.sh`, `time.sh` | Instructions and indirect branches under Callgrind; nanoseconds per call |
| `build_time.cpp`, `build_time.sh` | What `erase_handles` does to build time |

## The design

### `function`

```cpp
fetch_fn pass_store::fetch(str key) const {
    return fetch_fn(*this, std::move(key), [](const pass_store& self, str& key) noexcept {
        return body<pass_fetch_op, fetch_sigs>(self.inner, &key);
    });
}
```

`function<Sigs, Args...>` holds a pointer to the object, the curried
arguments and a pointer to a static descriptor generated from the
factory's type: the callee's frame size and alignment, and four entry
points.

| Entry point | Does |
|---|---|
| `connect(frame, obj, args, k, sp)` | Construct the callee's operation in a frame the caller pushed |
| `start(frame)` | Start it |
| `enter(frame, obj, args, k, sp)` | Both, in one call |
| `destroy(frame)` | Destroy it; null if there's nothing to destroy |

`k` is the caller's continuation record and `sp` the strand. The factory
must be stateless and `noexcept`, and it borrows the curried arguments,
which stay in the caller's `function` until the call has completed.

Connecting an lvalue `function` borrows it. A body does this: the awaited
call's `function` is a temporary that lives to the end of the
full-expression. Connecting an rvalue moves it into the operation. The
root and the statically composed algorithms do this.

The caller pushes the callee's frame on its strand, either at `connect`
(calling `connect`, then later `start`) or at `start` (calling `enter`).
`SLICE_ALLOC` chooses, for W-161. When the callee completes, the caller's
resume function destroys the callee's operation and pops its frame, then
carries on. So the frame is gone before the caller's code runs.

### The record and the destinations

The record is one pointer to a static table, with one resume function per
declared completion signature, in `Sigs`' order, and the stop token's query.
The strand isn't in the table: it is passed to the callee, which keeps it
in its receiver. Where each completion's payload goes is what `SLICE_DEST`
varies:

| Build | `kv-dest` row | The callee finds a payload's destination by | Filled |
|---|---|---|---|
| `CHAIN` | 4A | calling the table's destination function, which asks the caller's receiver then | never: asked when needed |
| `ONCE` | 4A, same ABI | calling the same function, which loads a pointer | at `start`, for every payload signature |
| `RECORD` | 3A | loading a pointer in the record | at `start` |
| `OUT` | 2A (5A's erased half) | loading a pointer from a table passed beside the record | at `start` |

In statically composed code every build uses the member template,
`destination<Sig>()`, which is the half of 5A they all share. A chain of
erased pass-through layers chains `CHAIN`'s calls, one per layer.

### The bodies

Each body is an operation state written as a compiler would lower its
direct-style source, given in `kv.hpp` beside it. The rules:

- An awaited call's `function` lives to the end of the full-expression,
  and its operation is destroyed when it completes.
- A returned local is constructed at the caller's destination, whose
  address is asked for once, when the local comes into existence.
- `co_return co_await f(...)` passes the callee's completions through.
- A `catch` for a typed error gives that error storage of the frame's own.
- A body may move from its parameters. They live in the caller's
  `function`.

| Body | Shape |
|---|---|
| `mem_store` | The leaf: `get` (an optional, never fails) and `fetch` (a string, or `not_found`), inline or from an event loop |
| `cache::get` | Hit: answers. Miss: awaits the backing store, reads the value, which is already at the caller's destination, inserts a copy, returns it |
| `with_default::get_or` | `try { co_return co_await fetch(key); } catch (not_found) { co_return fallback; }`: the value passes through, the error is filtered |
| `or_else` | The same, as `upon_error` composed statically above an erased `fetch` |
| `two_tier::fetch` | Falls back to a second backend on `not_found`: the first call's error is filtered, everything else passes through |
| `two_tier::get` | Falls back on an empty answer: a middle that filters a value |
| `pass_store` | `co_return co_await inner->fetch(std::move(key))`, in chains of 1, 2, 4 and 8 |
| `mem5_store`, `pass5_store` | A leaf with five completions (a value, three typed errors, stopped), under two pass-through layers |
| `pinned_default` | `get_or` for a type whose move constructor is deleted |

## Building

`cmake --workflow --preset gcc-debug` builds and runs everything with the
rest of the repository. `ctest -R '^slice\.'` runs just these: the checks
in all 32 configurations (four modes by four destination designs by two
allocation points), a short run of every bench scenario, and the
build-time chain at depth 16.

By hand, with `$STDEXEC` stdexec's `include` directory at 30acdb0:

```sh
g++ -std=c++23 -O1 -fsanitize=address,undefined -DSLICE_COUNT -isystem "$STDEXEC" \
  -DTH_MODE=3 -DSLICE_DEST=0 -DSLICE_ALLOC=0 checks.cpp stores.cpp -o checks && ./checks
./cg.sh g++ 1000; ./cg.sh clang++ 1000      # STDEXEC defaults to a preset's copy
./time.sh g++ 500000; ./build_time.sh g++
```

Measured with GCC 13.3 and Clang 18.1, libstdc++, at `-O2 -DNDEBUG`,
statically linked, on x86-64. The callees are in their own translation
unit, built without LTO. In the `gcc-debug` preset the checks run under
ASan and UBSan, and all 37 tests pass. By hand, every configuration's
checks also ran clean with GCC under ASan and UBSan (with
`-fno-sanitize-recover`). Clang ran without sanitizers, because the
sandbox lacks their runtimes. Both compilers build everything with
`-Wall -Wextra -Wpedantic` and no warnings.

## Observed

Instructions and indirect branches are the difference between 1000 and
3000 calls under Callgrind, per call. Each call includes the root: `run_slice`
over `just(7)` costs 44 / 23 instructions and 1 indirect branch. Each
cell is GCC (Clang), written instructions/indirect branches. Returns
aren't counted as indirect branches.

### Correctness, moves and storage

1. **Every scenario completes correctly in every configuration,**
   including an immovable value through a filtering middle (`pinned_*`)
   in all four destination designs. A two-tier miss, a deferred leaf, a
   five-way completion and an eight-layer chain all leave the strand
   empty.
2. **Results never move across a `function`.** A value is copied once,
   at the leaf, from the store into the caller's destination, then
   reaches the root through any number of layers without a move. That
   holds in `pass8_value`, `tier_secondary` and `get_or_value`, and on
   `get_or`'s error path the fallback is moved once, into place.
   (`get_or`'s value counts also include its fallback argument, which
   moves twice on the way in, like the key.) The
   synchronous versions move once where a value is returned from
   something that isn't the return object (`get_or`, `tier_primary`,
   `get_miss`: 1 value move each).
3. **Arguments move one more time per call than synchronously.** Each
   erased call moves its key twice: once into the callee's parameter, and
   once from the parameter into the `function`. The synchronous call moves
   it once (`pass8`: 18 key moves against 8, the root's owning `connect`
   included). The extra move is P4223's constructor: the interface's
   parameters are the virtual function's, and the `function` it returns
   must keep them.
4. **No heap allocation anywhere.** Every frame is on the root's 4 KiB
   strand: 0 `operator new` calls and 0 slabs in every scenario. Frame
   storage, the strand's high-water mark:

   | | CHAIN | ONCE | RECORD | OUT |
   |---|---|---|---|---|
   | `get_hit` | 136 B | 144 B | 144 B | 152 B |
   | `get_miss` | 192 | 200 | 200 | 216 |
   | `tier_secondary` | 288 | 320 | 320 | 336 |
   | `pass8_value` | 1,080 | 1,208 | 1,208 | 1,280 |
   | `five_value` | 288 | 368 | 368 | 392 |

   A cache hit pushes 136 B, because the caller pushes the frame for
   the whole body, room for the call it didn't make included. `ONCE` and
   `RECORD` add one pointer per payload signature per call, and `OUT`
   adds the table's pointer too.

### TYPED and VARIANT

5. **TYPED and VARIANT compile to the same code here.** Every scenario
   has identical instruction and indirect-branch counts in both modes, with
   both compilers. Every place where outcomes meet in these bodies is at
   a `function`: a leaf completes into the erased record, and a body's
   branch joins a typed completion with an erased start. The choice between
   the two modes shows only in statically composed code (as `typed-handle/`
   measured) and in build time (12 below).

### Destinations: W-163

VARIANT, frames pushed at `connect`:

| Scenario | Sync | CHAIN | ONCE | RECORD | OUT |
|---|---|---|---|---|---|
| `get_hit` | 243/4 (223/4) | 454/9 (463/11) | 456/9 (447/11) | 437/8 (461/10) | 452/8 (444/10) |
| `get_miss` | 352/6 (296/6) | 747/16 (775/19) | 751/16 (798/19) | 742/13 (786/16) | 737/13 (771/16) |
| `get_or_value` | 308/5 (295/7) | 718/14 (794/20) | 726/14 (803/20) | 733/12 (794/18) | 720/12 (795/18) |
| `get_or_error` | 234/3 (225/5) | 671/12 (739/19) | 683/13 (751/20) | 684/10 (737/17) | 671/10 (738/17) |
| `or_else_error` | – | 375/6 (475/11) | 379/6 (479/11) | 375/5 (475/10) | 374/5 (475/10) |
| `tier_secondary` | 455/9 (438/10) | 996/22 (1098/27) | 1020/24 (1121/29) | 1000/19 (1097/24) | 975/19 (1096/24) |
| `tier_get_secondary` | 456/9 (442/10) | 1029/24 (1121/29) | 1036/24 (1131/29) | 1022/20 (1126/25) | 1020/20 (1132/25) |
| `pass1_value` | 265/5 (259/6) | 651/14 (720/18) | 666/15 (733/19) | 654/12 (723/16) | 654/12 (722/16) |
| `pass2_value` | 324/6 (320/8) | 853/20 (968/26) | 880/22 (992/28) | 859/17 (974/23) | 860/17 (973/23) |
| `pass4_value` | 442/8 (442/12) | 1293/32 (1500/42) | 1344/36 (1546/46) | 1305/27 (1512/37) | 1308/27 (1511/37) |
| `pass8_value` | 678/12 (704/20) | 2173/56 (2564/74) | 2272/64 (2654/82) | 2197/47 (2588/65) | 2204/47 (2587/65) |
| `pass8_error` | 595/10 (614/18) | 2089/54 (2484/72) | 2188/62 (2574/80) | 2112/45 (2507/63) | 2119/45 (2506/63) |
| `five_value` | – | 849/20 (957/26) | 909/26 (1012/32) | 867/17 (983/23) | 865/17 (977/23) |
| `five_timeout` | – | 851/19 (902/25) | 913/25 (957/31) | 878/16 (928/22) | 871/16 (923/22) |
| `pinned_error` | – | 474/10 (542/15) | 486/11 (554/16) | 467/8 (527/13) | 464/8 (529/13) |

Nanoseconds per call (GCC / Clang), the best of seven runs of 500,000, on
a shared two-core VM, so differences under about 10% aren't findings:

| Scenario | Sync | CHAIN | ONCE | RECORD | OUT | CHAIN, start | RECORD, start |
|---|---|---|---|---|---|---|---|
| `get_hit` | 17 / 16 | 29 / 31 | 29 / 35 | 30 / 33 | 30 / 32 | 29 / 31 | 30 / 30 |
| `get_miss` | 27 / 23 | 48 / 51 | 49 / 58 | 57 / 50 | 52 / 51 | 48 / 54 | 50 / 49 |
| `get_or_error` | 20 / 23 | 48 / 56 | 48 / 63 | 47 / 53 | 48 / 52 | 47 / 52 | 47 / 50 |
| `tier_secondary` | 38 / 48 | 72 / 90 | 71 / 89 | 71 / 88 | 70 / 92 | 71 / 88 | 71 / 82 |
| `pass1_value` | 22 / 20 | 42 / 48 | 43 / 49 | 44 / 46 | 43 / 47 | 42 / 44 | 43 / 43 |
| `pass8_value` | 43 / 54 | 163 / 170 | 163 / 188 | 159 / 176 | 160 / 167 | 158 / 165 | 155 / 151 |
| `five_value` | – | 60 / 67 | 61 / 74 | 59 / 64 | 61 / 64 | 57 / 61 | 60 / 61 |

6. **4A as `kv-dest` built it (CHAIN) is within noise of the pointer
   tables.** Its instructions are within 4% of `RECORD`'s and `OUT`'s in
   every scenario, and fewer in the deepest chains. In `pass8_value` it costs 2,173 against
   2,197 and 2,204 with GCC, and 2,564 against 2,588 and 2,587 with
   Clang. It costs one more indirect branch per erased layer that a
   payload crosses: 56 against 47 at depth 8. No timing separates the
   three.
7. **Resolving once, up front, is worse than chaining, not better.**
   `ONCE` costs more instructions and more indirect branches than `CHAIN`
   in every scenario (`pass8_value` 2,272/64; `five_value` 909/26
   against 849/20). It answers every payload signature's lookup at
   `start` through the same table of functions, so it pays a call for
   completions that don't happen. With five completions, that's four
   lookups to save one. It's also the slowest design with Clang.
8. **The pointer tables pay at `start` too, but only loads and stores.**
   `RECORD` and `OUT` fill a pointer per payload signature at every call.
   With five completions they cost 18 instructions more than `CHAIN`
   (`five_value`, GCC), and they save one indirect branch per call. `OUT`
   and `RECORD` are indistinguishable.

### Allocating at `connect` or at `start`: W-161

VARIANT:

| Scenario | CHAIN, connect | CHAIN, start | RECORD, connect | RECORD, start |
|---|---|---|---|---|
| `get_hit` | 454/9 (463/11) | 449/8 (440/10) | 437/8 (461/10) | 430/7 (437/9) |
| `get_miss` | 747/16 (775/19) | 737/14 (749/17) | 742/13 (786/16) | 725/11 (739/14) |
| `get_or_error` | 671/12 (739/19) | 663/10 (692/17) | 684/10 (737/17) | 669/8 (690/15) |
| `tier_secondary` | 996/22 (1098/27) | 983/19 (1027/24) | 1000/19 (1097/24) | 985/16 (1029/21) |
| `pass1_value` | 651/14 (720/18) | 641/12 (675/16) | 654/12 (723/16) | 645/10 (677/14) |
| `pass8_value` | 2173/56 (2564/74) | 2121/47 (2379/65) | 2197/47 (2588/65) | 2153/38 (2402/56) |
| `five_value` | 849/20 (957/26) | 838/17 (894/23) | 867/17 (983/23) | 859/14 (915/20) |

9. **Allocating at `start` saves one indirect call per erased call.**
   With nothing between connecting and starting, `connect` and `start`
   fold into one entry (`enter`). That saves 5 to 7 instructions per call
   with GCC and 20 to 25 with Clang (`pass8_value`: 2,564 to 2,379). The
   timings mostly agree, by a few percent. Frame storage doesn't change,
   because the strand is LIFO either way. What the choice costs
   elsewhere wasn't exercised: no allocation fails here, so nothing
   measures an allocation failure becoming a completion.

### Against the synchronous call

10. **The asynchronous calls cost 1.9 to 3.6 times the synchronous ones'
    instructions, and 1.7 to 3.8 times their time.** Per erased pass-through layer
    (`pass8_value` less `pass1_value`, over 7), GCC spends 217
    instructions and about 6 indirect branches, against 59 and 1
    synchronously.
11. **About half of each layer is the argument.** Callgrind's
    per-function costs, `pass8_value` less `pass1_value` (GCC, `CHAIN`,
    at `connect`), per layer:
    - about 100 instructions in `std::string`: two moves and the
      destructors of the moved-from parameter and `function` member,
      where the synchronous call makes one move;
    - 58 in the generated entry points;
    - 29 in the caller's `start` and resume function;
    - 21 in the strand's push and pop.

    The indirect branches are the virtual call (which the synchronous
    call also makes), `connect`, `start`, the destination lookup, the
    resume function, and `destroy`. That last one does nothing: at
    completion a body has already destroyed everything it owns, but its
    type's destructor isn't trivial. (`kv-handle/`'s hidden entry cost
    about 59 instructions more than synchronous per call. Its factory
    took the key by reference rather than as P4223's by-value parameter,
    and its bodies differ, so the two aren't comparable line for line.)

### Build time and `erase_handles`

`build_time.cpp`: a chain of `then`s whose functions may throw, so every
level is a place where two outcomes meet, with an `erase_handles` every 8
or 4 levels. Seconds to compile and link at `-O2` (one run each),
instructions and indirect branches per run:

| Mode | Depth | No firewall | Every 8 | Every 4 |
|---|---|---|---|---|
| TYPED, GCC | 32 | 2.1 s; 958/31 | 1.8 s; 1021/36 | 1.9 s; 1065/40 |
| TYPED, GCC | 64 | 5.7 s; 1903/63 | 3.1 s; 2012/72 | 3.4 s; 2100/80 |
| VARIANT, GCC | 16 | 1.7 s; 176/0 | 1.5 s; 249/2 | 1.2 s; 301/4 |
| VARIANT, GCC | 32 | 6.1 s; 329/0 | 1.9 s; 456/4 | 1.8 s; 560/8 |
| VARIANT, GCC | 64 | 73.9 s; 672/0 | 4.7 s; 867/8 | 3.9 s; 1077/16 |
| TYPED, Clang | 64 | 6.8 s; 1347/64 | 5.0 s; 1430/72 | 5.5 s; 1504/80 |
| VARIANT, Clang | 32 | 5.9 s; 623/0 | 2.5 s; 648/4 | 2.5 s; 664/8 |
| VARIANT, Clang | 64 | 41.7 s; 1263/0 | 6.6 s; 1300/8 | 6.6 s; 1326/16 |

At depth 8 every point takes about 1 s. The full table is
`build_time.sh`'s output.

12. **VARIANT's build time grows much faster than its depth. A firewall
    every 8 levels brings it back to TYPED's or below.** Without one, a
    VARIANT chain takes 1.0, 1.7, 6.1 and 74 s at depths 8, 16, 32 and 64
    with GCC (1.1, 1.8, 5.9, 42 with Clang). TYPED takes 1.1 to 5.7 s
    (1.0 to 6.8). With `erase_handles` every 8 levels, VARIANT at depth
    64 takes 4.7 s with GCC and 6.6 s with Clang.
13. **The firewall costs one indirect branch per crossing.** It also
    costs some instructions: 29% with GCC (672 to 867 at depth 64), 3%
    with Clang (1,263 to 1,300). Even so, it stays well below TYPED,
    which makes an indirect call at every level (1,903/63 and
    1,347/64). A firewall every 4 levels saves little more build time
    than one every 8, and costs more instructions.

### The workload that doesn't apply

14. **The entry's signature is fixed, whatever the function's arity.**
    The hand-off's "one more parameter than `get_or`" workload tested
    register pressure in an entry that took the arguments themselves.
    Here the curried arguments reach the callee through the `function`,
    as one pointer to their tuple. `connect` and `enter` take five
    arguments, six in `OUT`, which then uses all six x86-64 integer
    argument registers. So anything added to the entry's ABI would spill
    in `OUT` first (inference).

## Reading

1. **The decision tree's third step comes out "yes": 4A.** Chaining
   destination functions through pass-through layers is within noise of
   loading a pointer, in instructions and in time, at least to a depth of
   8 here. "Resolving once, up front", the form the tree expected to make
   4A competitive, turns out to be unnecessary and is a loss. A frame
   that already knows where its result goes (a returned local, saved when
   it comes into existence) loads it, and that's a frame's own choice
   behind 4A's ABI, as the hand-off argued. 4A also keeps the smallest
   record and frames. Its price is one indirect call per layer a payload
   crosses. That stays cheap only while indirect calls are cheap, so the
   tree's per-platform caveat (CFI, retpolines) stands and wasn't
   measured.
2. **W-161 leans to `start`, on cost:** one indirect call fewer per
   erased call, and no other difference measured here. The case against
   (allocation failure as a completion, P4223 §9) is semantic, and the
   slice doesn't test it.
3. **What makes an erased call expensive isn't the destination design.**
   The rows differ by at most 4%, while the call costs about 160 instructions
   more than a synchronous one. The largest part is P4223's argument
   handling: the parameter moved into the `function`, and two dead
   strings to destroy. The rest is the generated entry points, the strand,
   and a `destroy` call that does nothing. Each of these is a separate
   question: arguments in place across the interface (W-145); a callee
   that ends its own lifetime at completion, so that the caller only pops;
   and whether `connect` and `start` stay two entries.
4. **VARIANT against TYPED is a question about composed code, and the
   firewall answers its build-time objection.** Behind `function`s the
   modes are the same. In composed code, VARIANT's build time is the
   cost, and `erase_handles` contains it for one indirect call per
   crossing. TYPED offers no such trade: it erases at every merge.

## What it doesn't show

- The C++26 baselines in the same binary: P4223R2's `function` and
  `task`, which need stdexec 5afe072. `kv-handle/`'s numbers are from
  another binary and other bodies.
- One C++26 sender inside the slice through the generic adaptor, and
  `just` calling a defaulted `set_value` (the roadmap's other two
  variables).
- A compiler. Every body is lowered by hand, and the bodies'
  operation states are C++ objects whose destructors the protocol still
  calls.
- Concurrency: one strand; no forks, no completion on another thread, no
  cancellation crossing a `function` (the stop token is forwarded, and
  `five_stopped` only produces stopped).
- Allocation failure, slab overflow (no scenario needs a second slab),
  and deep call trees.
- Code size of the bodies, MSVC, other architectures, CFI or retpolines,
  and a machine that isn't a shared VM.
- The P4014 acceptance suite (W-151).
