.data

_deno_snapshot_start: .incbin "gen/snapshot_deno.bin"
_deno_snapshot_end:
.globl _deno_snapshot_start;
.globl _deno_snapshot_end;
