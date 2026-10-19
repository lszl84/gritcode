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

One running turn per working folder. Starting a turn while another thread in
the same folder is running queues it (shown as "waiting for <thread>"). A
worktree thread's checkout is a working folder of its own (`Thread::workDir`,
see "Worktree threads"), so it runs alongside the project folder's threads.

A thread and its subagents count as one owner of the folder, so a subagent
never waits for its own parent. Subagents are read-only: `write_file`,
`edit_file` and `run_project` are neither offered to them nor run for them,
and their prompt tells them to use bash only to inspect (a convention, not
enforced). The parent makes the edits from their reports, so concurrent
subagents never write under each other.

## Subagents

A subagent is a child thread (`parent` set), one level deep, view-only, listed
under its parent in the dropdown and linked from the parent's tool block.

- DeepSeek/Kilo: top-level threads get a `task` tool (description, prompt).
  Each call opens a child thread in the same folder with a fresh context (base
  system prompt plus a subagent note) that runs headless. A Pro parent's child
  uses the matching Flash model, otherwise the parent's model. The calls of
  one batch run at once, alongside the batch's other tools; the batch ends when
  the tool worker and all children are done, and the results go into the
  parent's history in call order: the child's final message, or the failure.
  Cancelling the parent cancels its children; cancelling a child alone gives
  its parent an error result. A failed child keeps its history.
- Claude Code: an `Agent` (older CLIs: `Task`) tool call opens a child thread
  titled by its description, and events with that `parent_tool_use_id` are
  assembled into it by the same code as the top level. It ends with the call's
  result, or, for a background agent (the result only says it started), with
  `system/task_notification`; a failure is also appended to the parent's
  recorded result. Cancelling a child cancels the parent's Claude Code run;
  children still running when that run ends are marked stopped.
- The child's id is recorded on the parent's tool call (`"thread"`, kept off
  the wire), so restored parents still link to their children.

## Storage (no migration)

Session files keep their format: `sessions/<id>.json` with `id`, `cwd`,
`lastUsed`, `messages`, plus optional `title`, `parent`, and for worktree
threads `worktree` (the checkout's root) and `branch`; the index's `threads`
rows carry the same optional fields. Old versions ignore them and show a
worktree thread as an ordinary thread of its folder. The existing
session of a folder (id = hash of cwd) simply becomes its first thread. The
index keeps `sessions` (one entry per folder, exactly what 0.5.x reads) and
adds `threads` for additional threads, which 0.5.x ignores. Downgrading keeps
working: old versions see every folder's first thread with full history.

## Project brief

A thread starts without the history of the folder's other threads. The brief
gives it the background instead: a few paragraphs on what the project is, its
layout and conventions, and the current work. `SystemPrompt` composes the
base prompt, AGENTS.md, the brief and a list of the folder's other titled
threads (title and day last used, at most 10). Titles and days only: running
state changes every turn and would keep changing the cached prompt prefix. A
thread's prompt is rebuilt when it opens, when AGENTS.md changes and when the
brief is saved; Claude turns get the same text with each run.

Storage: `briefs/<folder id>.md` in the app data dir, next to `sessions/` and
keyed like a folder's first thread. Gritcode doesn't write into the user's
repo unasked, and AGENTS.md already is the versioned, in-repo place for
project instructions (it is injected too). No brief means no section, as
before.

"Project brief…" in the thread dropdown opens a dialog to edit it. "Generate"
asks DeepSeek Flash (cheap and fast; the free model without a key; Claude's
CLI doesn't fit one request) to write it from the top-level entries, README,
AGENTS.md and thread titles. The text streams into the box; failures show in
red and restore the previous text. Nothing is generated unasked: a new thread
in a folder with other threads and no brief only shows a one-line hint
linking to the dialog.

## History search

Every thread's turns are indexed in `memory.db` under its own id when it is
saved, and `--reindex` walks every session file. `grit_history_search` leaves
out the calling thread (its history is already in context), takes `scope:
"folder"` to keep to the thread's folder (default: all folders), and shows
each hit's thread title, marking subagents. Over `--mcp-stdio` the folder is
the server's working directory and nothing is excluded. The tool description
and the "other threads" list in the prompt point a fresh thread at its
folder's other threads.

Storage: titles and parents live in a new plain table `threads(session_id,
title, parent)` next to the FTS5 `turns` table, which keeps the exact 0.5.x
schema. 0.5.x only creates, reads and writes `turns`, so it ignores the new
table, and old rows need no rebuild: a session without a `threads` row (one
indexed by 0.5.x or before this step) shows no title until its next save or
a `--reindex`. Nothing is deleted except a session's own rows on its
rebuild, as before; turns of sessions with no file any more stay searchable.

## Worktree threads

A worktree thread writes in its own git checkout, so several threads can
change one project at once. "New thread in worktree" (dropdown entry and pane
button, offered only when the folder is in a git repo) runs
`git worktree add -b grit/<first 8 of the id> <data dir>/worktrees/<id> HEAD`:
the checkout lives outside the user's repo and holds the committed state (the
creation notice says that uncommitted changes aren't in it). The thread still
belongs to its folder (`cwd`, listed there, same brief and AGENTS.md), while
its tools, bash and Claude Code run in `workDir`, the same subfolder of the
worktree as the folder is of the repo. Its subagents get its `workDir`, so the
family rule holds as before. Its system prompt adds that it works in an
isolated worktree of the project on its branch. History search's "folder"
scope stays the project folder. It is saved as soon as it is created, so its
checkout is never unlisted.

Its canvas starts with a header (branch, "Review changes", "Merge into
<branch>", "Discard worktree"), the pane and dropdown rows show the branch,
and the pane's right-click menu offers the same actions. The git work is a
few free functions in `git_worktree.{h,cpp}` that run git synchronously with
an argv (fork/exec, no shell); they are quick, and run only on the user's
click.

- Review: `git add -A` in the worktree (its index is the thread's), then
  `git diff --cached <merge-base of the worktree and the project's HEAD>`:
  committed and uncommitted changes, new files included, as a stat notice and
  a diff code block in the canvas (not in history, so the model doesn't see
  it).
- Merge: refused while the thread (or a thread in the project folder) runs,
  when the project checkout has uncommitted changes to tracked files, or when
  it is on no branch. Otherwise the worktree's uncommitted changes are
  committed on its branch with the thread's title as the message, then
  `git merge --no-ff grit/<id>` in the project checkout. A conflict is
  aborted with `git merge --abort`; the notice names the files and suggests
  merging the project's branch into the worktree branch in the thread
  (conflicts are then resolved in its isolated checkout). Every outcome,
  including the commit message used, shows as a notice; if the abort itself
  fails, the notice says the repo is mid-merge and what to run.
- Why a merge commit rather than applying the diff as uncommitted changes:
  the merge is refused unless the checkout is clean, so the user's own work
  is never mixed in; a conflict aborts back to exactly the previous state
  (`git apply --3way` would leave conflict markers among the user's files);
  and the result is one commit, easy to inspect and undo
  (`git reset --hard HEAD~1` or `git revert -m 1`). The worktree stays, so
  the thread can go on and be merged again.
- Discard: refused while the thread runs. When the branch has commits not
  in the project's HEAD, or the worktree has uncommitted changes (or git
  can't tell), it asks first and names what would be lost. Then
  `git worktree remove --force` (or `git worktree prune` when the checkout is
  already gone) and `git branch -D`. The thread stays, as a thread of the
  folder itself.
- A worktree deleted by hand: the header says so and offers Discard (which
  prunes it and deletes the branch); sending a message is refused with a
  notice.

There is no "delete thread" yet; when there is, it must discard the
worktree the same way.

## UI

The session dropdown lists the current folder's threads (running ones marked,
subagents indented under their parent) plus "New thread", "New thread in
worktree" (git repos only), "Project brief…" and "Open folder…".

The thread pane (toolbar toggle, remembered in preferences) shows the same
rows at the far left: a "New thread" button, then per thread a state mark
(● running, ○ waiting for the folder, ⚠ failed until viewed) with the title,
and a dim line below: while running the current step (the tool it runs,
"writing", "thinking", "waiting for <thread>"), after a failure its notice,
otherwise when it was last used. Both are built from one row list
(`FolderRows`), so they always agree; the step is derived from the thread's
existing fields (pending tool calls in history or Claude's pending message,
in-flight text), and a 1 s timer refreshes the pane only while a thread of
the folder runs. Clicking a row selects the thread, right-click on a running
one offers "Stop". The pane is a fixed-width panel beside the splitters (no
sash): opening it grows the window by its width, or shares a tiled window,
like the other panes. The agent map and the decision tray come later (see
`orchestrator-conversation.md`).

## Steps

1. Threads: struct, multi-thread runtime, store, per-thread cwd, write queue,
   dropdown. (done)
2. Subagents: a DeepSeek `task` tool running a child thread headless, and
   Claude Code subagent events shown as child threads. (done)
3. Fixes that matter once threads run at once (found on the old branch): a
   bash timeout that kills the whole process group, a lock around run-config
   read-modify-write, SSE parsing of CRLF streams / a final event without a
   blank line / `data:` without a space, and "Refresh" in the file tree saving
   the open file.
4. Project brief: a short per-folder brief (written or generated) and a
   list of the folder's other threads in every thread's system prompt.
   (done)
5. Thread pane: an optional left list of the folder's threads and their child
   threads with state (running, waiting, failed, done). (done)
6. Worktree threads: a thread can run in its own git worktree, so several
   threads write in one project at once; diff and merge back. (done)
7. Project-scoped history search for the memory tool. (done)

The orchestrator ideas (`orchestrator-conversation.md`) build on 2 and 5.
