# Threads: lean design

Goal: many independent conversations ("threads") per project folder, able to
run at the same time, later with subagents. Small, fast, data-oriented.

Rules: plain structs and free/member functions on data. No new class
hierarchies, virtual interfaces, managers, executors or event buses. Use C++20
and the standard library well. Aim for the simplest clean solution that reads
naturally; no code golf, and no speculative generality.

## Data

- `struct Thread` (in `chat_frame.h`) holds what is per-conversation state
  today as `ChatFrame` members: id, cwd, title, parent id, history, the
  in-flight `StreamingWebRequest`, `ClaudeAgentProcess`, tool worker and
  cancel token, SSE and tool-call buffers, compaction state, Claude turn state,
  queued messages, flags (streaming, compacting, ...), plus `uint32_t gen`.
- `ChatFrame` owns `std::deque<Thread> threads_` (stable addresses on
  push_back) and `size_t active_`, the thread shown on the canvas.
- Process-wide state stays in `ChatFrame`: model list, preferences, memory DB,
  editor, canvas, persist worker.

## Async results

Every callback captures `(thread id, gen)` instead of relying on members:
`[this, id, gen](auto... a) { if (Thread* t = Find(id, gen)) OnX(*t, a...); }`.
`gen` is bumped whenever a thread's request/process/worker is replaced or the
thread is removed, so stale replies are dropped. This also fixes the existing
"queued reply after the request was destroyed" hazard.

## Rendering

Only the active thread draws to the canvas. Background threads update data
only. Switching threads rebuilds the canvas from history (as `RestoreSession`
does today) plus the in-flight partial text.

## Tools

`DispatchTool` gets the thread's cwd: relative paths resolve against it and
bash `chdir`s in the forked child. The process-wide `ChdirToCwd` goes away.

## Write safety

One running turn per folder. Starting a turn while another thread in the same
folder is running queues it (shown as "waiting for <thread>"). Real parallel
writing in one folder comes later with worktrees.

## Storage (no migration)

Session files keep their format: `sessions/<id>.json` with `id`, `cwd`,
`lastUsed`, `messages`, plus optional `title` and `parent`. The existing
session of a folder (id = hash of cwd) simply becomes its first thread. The
index keeps `sessions` (one entry per folder, exactly what 0.5.x reads) and
adds `threads` for additional threads, which 0.5.x ignores. Downgrading keeps
working: old versions see every folder's first thread with full history.

## UI

The session dropdown lists the current folder's threads (running ones marked)
plus "New thread" and "Open folder…". A left thread pane, the agent map and
the decision tray come later (see `orchestrator-conversation.md`).

## Steps

1. Threads: struct, multi-thread runtime, store, per-thread cwd, write queue,
   dropdown.
2. Subagents: a DeepSeek `task` tool running a child thread headless, and
   Claude Code subagent events shown as child threads.
3. Worktree threads (only when the UI uses them).
