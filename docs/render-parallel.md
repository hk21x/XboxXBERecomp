# D3D12: recording draws on several threads (design, render-mt)

Why: PGR2's heaviest race seconds send about 4500 to 5700 draws a frame. At about 3 us a draw the render thread then
needs 14 to 16 ms a frame (60 fps); 120 needs under 8.3 ms. Measured on Harry's PC with XBE_PERF (PGR2 PROGRESS.md,
render thread performance). Cheap trims are spent: draw merging saved about 12% of draws, reusing vertex ranges nothing.

Where a heavy frame's draw time goes (4556 draws, 14.2 ms): assemble 2.1, bind 2.2, consts 1.7, tables 1.3,
geometry 3.2, shaders and pipeline 1.2, setup 0.2.

## The split

Each draw becomes two halves.

- **Plan** (the render thread, in command order, as today): everything that reads or writes the backend's shared state.
  BindTargets and the transitions it records; BindStageTexturesAndTargets (stage memo, textures_, uploads, target
  reloads and resolves, LatchCpuWrite); shader and pipeline lookup and creation; samplers; ext_ hooks; captures. It also
  sizes and allocates what the draw will write: its upload ranges (vertices, indices, constants) and its descriptor slots.
  Its result is a packet: the PSO, root signature, render target and depth views, viewport and scissor, the descriptor
  sources and their destination slots, the upload ranges with the guest memory to fill them from, the draw call.
- **Record** (a worker): fills the upload ranges (vertex conversion or copy, index copy, constants), copies the
  descriptors, and records the packet's commands into the worker's command list.

Packets of one batch are cut into runs. Each run is recorded by one worker into a command list of its own; the lists
go to the queue in order, so the GPU sees exactly the commands it sees today, in the same order. A command list starts
with no state, so each run's first packet sets everything (root signature, heaps, targets, viewport, PSO); after that
the worker skips what the last packet set.

Barriers stay in order: Plan emits them as packets of their own, recorded by whichever run they fall in. Anything that
must see the GPU's results first (Sync and read-backs, Present, copies that the CPU waits for, a mid-frame Submit when
the upload ring fills) is a fence point: Plan waits for the runs so far to be recorded and submitted.

## Stages

- **A**: the split on one thread. Draw = Plan then Record at once, into cl_. Same pictures and same speed, checked on
  Windows. This is where the shared state is found and moved into Plan.
- **B**: Record on worker threads, a command list and allocator per worker per frame in flight; runs of about 256
  draws; fence points. Expected: a heavy frame from about 14 ms to 6 to 7 ms with four workers.
- **C**: tuning (run size, worker count from the CPU), then D3D11 (deferred contexts) if worth it.

Every stage: the six-game gate, Harry's XBE_PERF run of PGR2 at 120, and EoN with its blended frames on (gpu/interp.cpp
replays draws through Draw).

## Hazards to keep in mind

- Guest memory is read in Record, later than today. A CPU write between Plan and Record would be seen; today's code
  reads it during Draw, already after the title recorded the batch, and the render thread's resource tracking (Use,
  BlockOnResource) still holds: Record finishes before the batch counts as done.
- The upload ring and descriptor heaps are allocated in Plan only; Record writes into ranges it was given.
- Feedback copies, MSAA resolves and shrinks record commands in Plan (they are part of binding): they become packets.
- Occlusion queries (visibility tests) and captures stay on the render thread: a draw with either is recorded in Plan.


## Revision (2026-11-08): what stage A showed, and the pipeline instead

Stage A's recorder showed that writing the commands (the replay) is only about 1.1 to 1.4 ms of a heavy frame: stage B
as above (replay on helpers) gains little. Since then on render-mt and render-mt2:
- the prep thread assembles draws ahead (1.5 ms);
- the title's state compare is faster;
- textures are placed in heaps, which ended the in-race hitches (0.1% low 34 to 52 fps);
- reflections rest in late frames.
Harry's heaviest seconds are now about 80 fps at about 4600 draws, 2.2 us a draw.

A heavy frame's draw time (5114 draws, d8a6414):

| step | ms | where it belongs |
|---|---|---|
| targets, texture binding, shaders, pipeline | 0.3 + 2.5 + 0.8 + 0.4 | decide: shared caches (targets_, textures_, pipelines), barriers |
| constants (fill, compare, upload) | 2.1 | record |
| tables (views, samplers) | 1.0 | record |
| viewport, scissor, blend, stencil | 0.2 | record |
| geometry (vertex and index upload, the draw) | 0.5 | record |
| the draw's note, checks | 0.5 | record |
| replay into the list | 1.4 | record |

So a three stage pipeline, one draw at a time and in order:

1. **prep** (exists): its own state copy, assembly.
2. **decide** (the render thread): its own state copy; targets, textures, shaders, pipeline. Out: a packet per draw.
   It carries the pipeline, the stages (DrawTextures), the shader facts (DrawShaders, PixelKey bits), PsoKey's
   zbias, the bound targets' sizes and pointers, and the barriers and copies binding recorded.
3. **record** (a new thread): its own state copy, from the same batch changes; constants, tables, viewport and
   scissor, blend and stencil, geometry, note; it writes rec_ and replays it. It owns ds_cache_, root_args_,
   vsc_last_ and dc_last_, views and tables_, the samplers, and the draw notes.

Shared between decide and record, split or guarded:
- the upload ring: texture uploads (decide) and constants or geometry (record). Either two rings, or record owns
  the ring and decide's uploads become packets.
- descriptor slots: AllocSrvSlots (tables, record) and the targets' and feedback views (decide).
- anything decide writes to the list directly (copies, resolves, clears, presents, read-backs, Submit): a fence
  point. Decide waits until record has drained, does it, and goes on. Clears, copies and presents are already their
  own commands; binding's copies (cube faces, resolves, shrinks) go into the packet as recorded commands.

Steps, each verified on Harry's PC before the next:
1. The split on one thread: Draw becomes Decide (a packet) then Record (the packet), at once. Same pictures; the
   packet's cost is measured.
2. Record's own state copy from the batches (Threaded hands record the same changes); still one thread.
3. Record on its own thread, with fence points.
Expected: decide about 4.5 to 5 ms and record about 5 ms of today's heavy frame, run side by side: about 140 fps
in Harry's heaviest seconds.
