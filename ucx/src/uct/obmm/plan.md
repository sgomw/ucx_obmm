# OBMM CC probe plan

## Goal

Build probe programs before redesigning the UCT transport. The probes must
measure whether a CC-only protocol can get useful throughput from cacheable
OBMM memory while respecting the ownership model:

- writer owns a page range with `PROT_WRITE`
- writer stores payload, optionally executes a selected fence
- writer releases the range to `PROT_NONE`
- reader acquires `PROT_READ`
- reader consumes payload
- reader releases the range to `PROT_NONE`

The probe suite must not reuse the legacy NC FIFO data path, NC mappings, LSE
atomics, or bus-fence assumptions.

## Probe scope

1. `flip`: isolate permission transition cost for `obmm_set_ownership()` and
   dynamic `mmap()/munmap()`.
2. `touch`: measure local read/write bandwidth while repeatedly acquiring and
   releasing a CC mapping.
3. `handoff`: measure two-node writer-to-reader handoff over matching export
   and import shmdevs. TCP is used only as the out-of-band control channel; all
   payload bytes move through OBMM CC memory.
4. `list`: print local OBMM shmdevs from sysfs to make choosing memids less
   error-prone.

## Design critique

- A shared receive FIFO would be illegal under the CC model because it requires
  concurrent writer and reader access to the same OBMM region.
- The first useful measurements are not UCX AM measurements. They are primitive
  measurements: permission transition latency, access bandwidth while a side
  owns the region, and end-to-end handoff efficiency as chunk size changes.
- The required store fence before releasing ownership is not yet established.
  The probe therefore exposes `--fence none|seq_cst|arm_ish|arm_osh` instead of
  baking one policy into transport code.
- PMD huge mappings are intentionally not used because OBMM documentation says
  PMD mappings do not support `obmm_set_ownership()`.
- The handoff probe serializes ownership with a TCP control message. This avoids
  illegal concurrent access and gives a conservative baseline for protocol
  chunk-size decisions.

## Verification limits

This Windows workspace has no OBMM hardware and no Linux UCX toolchain. Local
verification is limited to static review. Compile and runtime validation must
be done on the two-node Linux OBMM setup.
