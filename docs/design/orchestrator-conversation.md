# Orchestrator conversation model (ideas, not decided)

Problem: while work is running, the user wants to keep talking to the
orchestrator. A common answer is a server-side message queue plus
"steer" (inject a queued message into the active turn) and "interrupt". That
makes the user classify every message's urgency. These are alternatives to
explore for Gritcode's thread runtime.

## 1. Orchestrator that's always free to talk
The orchestrator never does long work itself; it delegates everything to
worker threads. It is therefore never mid-turn, so messages to it never queue.
Queue/steer only exists at the worker level, and the orchestrator decides
which applies, not the user.

## 2. Route by intent instead of a mode button
The user just talks. The orchestrator works out what each message is:
- question → answered from the current state of the workers, no worker disturbed
- correction → passed to the affected worker immediately
- new work → a new thread or card
- change of plan → existing tasks updated, and the workers told
It shows one line saying what it did ("→ sent to *storage migration*"),
so a wrong guess is easy to spot.

## 3. Steer through shared state, not messages
Workers re-read a shared plan/board file (e.g. `.grit/board.md`) at every
step. The user or the orchestrator edits the plan; workers pick up the change
at their next step. Quieter than interrupting, leaves a record, and links up
with the planned kanban (cards hold their own instructions and limits).

## 4. Workers confirm corrections
A worker states its current step and confirms each correction ("got it,
switching after this edit"), so the user knows the correction landed.

## 5. Two separate streams
The conversation with the orchestrator stays clean; worker progress appears as
a status feed in a side pane, not mixed into the chat. Fits the default
chat-window-plus-optional-panes UI.

## 6. The orchestrator as a visible, personable guide
Luke's idea: the orchestrator is a named host for the project, not a hidden
mechanism. It greets you, e.g. "Hi! I'm the orchestrator for this project. I'll
start and watch your agents, report progress and guide you through the work.
Meanwhile, ask me anything about the project."
- It is the default view: the plain chat window *is* the orchestrator, and
  worker threads appear as things it manages.
- Personality should come from usefulness (proactive status, raising
  decisions, explaining what each agent is doing), not from chatter. Tone
  could be user-configurable.
- Cheap model for orchestration (it mostly routes and summarises), strong
  models for workers. This fits DeepSeek's prices: Flash as host, Pro or
  Claude as workers.
- It decides when to interrupt you (decisions, failures, done) versus when to
  stay quiet. Batching notifications is part of its job.
- The main risk is the orchestrator getting chatty or making decisions it
  should hand to you. It needs a clear rule: it proposes, you decide anything
  hard to undo.

## 7. Agent map: a zoomable radial view of agents
Luke's idea: instead of a lineage list, draw the agents
as a graph. The orchestrator is a circle at the centre, worker threads are
circles around it, and their subagents branch out as smaller circles. You can
zoom out for the big picture and zoom in on one branch.
- **Semantic zoom:** zoomed out, each circle shows only its state (colour,
  pulse while running); in the middle range, name plus current step; zoomed
  in, a live tail of the transcript and buttons (open, steer, cancel).
- **Visual encoding:** circle size = tokens or cost so far; an outer ring =
  progress or remaining budget; colour = state (running, waiting for the write
  lock, needs you, done, failed). Edges briefly light up when a result flows
  back to the parent.
- **Linking:** clicking a circle selects that thread in the chat; it is the
  same thread model as the thread list, drawn differently. Circles that need a
  decision from you stand out, so the map doubles as an inbox.
- **Feasibility:** Gritcode already draws its chat on a custom canvas
  (`chat_canvas.cpp`), so a native wxGraphicsContext view with a radial tree
  layout and pan/zoom is realistic, with no web view needed. It belongs in a
  side pane (IDE phase), built on WP5c's child-thread data.
- Possible prior art to look at: Gource (a radial, animated tree of a
  repository's history), d3's zoomable circle packing and radial trees, and the
  graph views in agent-workflow tools such as LangGraph Studio.

## 8. Decisions never get buried
Luke's observation: in a chat-only orchestrator, open questions and
decisions for the user scroll away and get lost in the conversation.
- Pending decisions are first-class objects, not chat text: a pinned
  "Needs you" tray (or the most prominent nodes on the agent map, idea 7)
  that stays visible until each one is resolved.
- Each decision card shows: the question, who is blocked by it (which
  threads/packages, and from which wave), the recommended option with
  one-line trade-offs, and one-click answers ("defaults OK" for a batch).
- Cards carry a deadline in workflow terms ("blocks wave E"), so the
  orchestrator keeps working on everything that isn't blocked and only
  escalates when a card actually blocks progress.
- Answers are recorded with the decision (an audit trail in the project, e.g.
  `.grit/decisions.md`) and fed to the affected workers automatically.
- Layout: the agent map plus the "Needs you" tray in front, and the chat as
  a side or bottom panel for asking questions, which turns the current
  layout around once orchestration is in use.

## Gritcode advantage
A single native process holds all threads, so the orchestrator can read every
worker's state directly (no server or remote calls in between). Answering
questions from current state is fast and cheap.

## What the thread runtime needs to allow these
- Accept input at step boundaries (between model calls / tool calls) for both
  the DeepSeek loop and the Claude stream-json driver.
- Plan/board state shared between threads and readable by tools.
- A per-thread "current step" status the orchestrator can query.
