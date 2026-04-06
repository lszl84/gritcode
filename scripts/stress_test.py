#!/usr/bin/env python3
"""
FCN Stress Test — intense multi-phase agent testing.
Sends increasingly demanding tasks and monitors for stalls.
"""

import sys, time, json, subprocess
sys.path.insert(0, "/home/luke/Developer/fastcode-native/scripts")
from mcp_client import FCN

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
TIMEOUT = 600  # 10 min per phase
STALL_THRESHOLD = 90  # seconds without progress = stall

fcn = FCN(port=PORT)
fcn.connect()

def run_phase(name, prompt, timeout=TIMEOUT):
    print(f"\n{'='*70}")
    print(f"PHASE: {name}")
    print(f"{'='*70}")
    print(f"Prompt: {prompt[:120]}...")

    fcn.send(prompt)
    start = time.time()
    last_msgs = 0
    last_progress = time.time()
    stalled = False
    peak_rounds = 0

    while time.time() - start < timeout:
        time.sleep(3)
        try:
            s = fcn.status()
        except Exception as e:
            print(f"  CONNECTION ERROR: {e}")
            return False

        elapsed = int(time.time() - start)
        rounds = s["toolRound"]
        msgs = s["messageCount"]
        busy = s["requestInProgress"]
        peak_rounds = max(peak_rounds, rounds)

        # Track progress
        if msgs != last_msgs:
            last_msgs = msgs
            last_progress = time.time()

        # Stall detection
        stall_time = int(time.time() - last_progress)
        status = "WORKING" if busy else "IDLE"
        if busy and stall_time > STALL_THRESHOLD:
            status = f"STALL ({stall_time}s no progress)"
            stalled = True

        print(f"  [{elapsed:3d}s] {status} | rounds={rounds} msgs={msgs} stall={stall_time}s")

        if not busy:
            break
    else:
        print(f"  TIMEOUT after {timeout}s")
        return False

    elapsed = int(time.time() - start)
    resp = fcn.last_assistant()
    text = resp.get("text", "")

    print(f"\n  Completed in {elapsed}s, {peak_rounds} tool rounds, {last_msgs} messages")
    print(f"  Response preview: {text[:300]}...")

    if stalled:
        print(f"  WARNING: Stall detected during this phase")

    return not stalled

def count_files():
    r = subprocess.run(
        ["find", "/home/luke/Developer/fcn-testbed", "-type", "f",
         "-not", "-path", "*/venv/*", "-not", "-path", "*/__pycache__/*",
         "-not", "-name", "*.db"],
        capture_output=True, text=True
    )
    files = [f for f in r.stdout.strip().split("\n") if f]
    return len(files), files

results = {}

# ─── PHASE 1: Build a substantial codebase from scratch ───
results["phase1"] = run_phase(
    "Build Full-Stack PHP Application",
    """Build a complete PHP task management application from scratch in the current directory:

Structure:
- public/index.php — front controller with routing
- src/Database.php — PDO/SQLite singleton
- src/Models/Task.php — Task model with CRUD, priorities, due dates, status
- src/Models/Project.php — Project model (tasks belong to projects)
- src/Models/Tag.php — Tag model with many-to-many relationship to tasks
- src/Controllers/TaskController.php — full REST: GET/POST/PUT/PATCH/DELETE, filtering by project/status/priority/tag
- src/Controllers/ProjectController.php — full REST with nested task listing
- src/Controllers/TagController.php — CRUD plus attach/detach from tasks
- src/Middleware/JsonResponse.php — sets headers, handles errors
- src/Router.php — simple regex-based router
- composer.json with PSR-4 autoloading

The database should have proper foreign keys, indexes, and a task_tags junction table.
After creating everything, test it with curl: create 2 projects, 5 tasks across them, 3 tags, attach tags to tasks, then query tasks filtered by project and by tag. Show the curl output."""
)

n_files, files = count_files()
print(f"\n  Files in testbed: {n_files}")
for f in sorted(files):
    print(f"    {f.replace('/home/luke/Developer/fcn-testbed/', '')}")

# ─── PHASE 2: Major refactoring ───
results["phase2"] = run_phase(
    "Major Refactoring — Extract Service Layer",
    """Refactor the codebase with these changes:

1. Extract business logic from controllers into service classes:
   - src/Services/TaskService.php — validation, business rules (e.g. can't close task with open subtasks), complex queries
   - src/Services/ProjectService.php — project stats (task count, completion %), cascade operations
   - src/Services/TagService.php — tag usage stats, bulk operations

2. Add input validation everywhere:
   - Task: title required (1-255 chars), priority must be 1-5, due_date must be valid ISO date or null, status must be one of: pending/in_progress/done
   - Project: name required (1-100 chars), description max 1000 chars
   - Tag: name required (1-50 chars), unique

3. Add pagination to all list endpoints: ?page=1&per_page=20, return X-Total-Count header

4. Controllers should be thin — just parse request, call service, return response

5. After refactoring, run the same curl tests from before to verify nothing broke. Also test validation by sending invalid data and showing the error responses."""
)

# ─── PHASE 3: Add Python test suite ───
results["phase3"] = run_phase(
    "Comprehensive Python Test Suite",
    """Create a comprehensive Python test suite:

1. Set up: python3 -m venv venv && source venv/bin/activate && pip install requests pytest

2. Create tests/conftest.py — pytest fixtures: start PHP dev server on a random port, create fresh DB, teardown

3. Create tests/test_projects.py — test all project CRUD, validation errors, pagination, cascade delete

4. Create tests/test_tasks.py — test all task CRUD, filtering by project/status/priority, validation (bad priority, bad date, empty title, title too long), pagination, status transitions

5. Create tests/test_tags.py — test tag CRUD, attach/detach to tasks, unique constraint, usage stats

6. Create tests/test_integration.py — complex scenarios: create project with tasks and tags, filter tasks by tag within a project, bulk operations, verify cascade deletes clean up junction table

7. Run: pytest -v tests/ and show the full output. Fix any failures until all tests pass."""
)

# ─── PHASE 4: Add new feature requiring understanding of existing code ───
results["phase4"] = run_phase(
    "New Feature — Task Comments & Activity Log",
    """Add two new features that integrate deeply with the existing code:

1. Task Comments:
   - src/Models/Comment.php — id, task_id, author, content, created_at
   - src/Controllers/CommentController.php — nested under tasks: GET/POST /tasks/{id}/comments
   - src/Services/CommentService.php — validation, author required
   - Route: /tasks/{id}/comments

2. Activity Log:
   - src/Models/ActivityLog.php — id, entity_type, entity_id, action, details (JSON), created_at
   - Modify TaskService, ProjectService, TagService to log all create/update/delete actions
   - GET /activity?entity_type=task&entity_id=5 — filter by entity
   - GET /activity?since=2024-01-01 — filter by date

3. Update the Python test suite:
   - tests/test_comments.py — full CRUD, validation, cascade delete when task deleted
   - tests/test_activity.py — verify logs are created for all operations, filtering works

4. Run the FULL test suite (all test files) and show results. Fix any failures."""
)

# ─── PHASE 5: Hardening — error paths and edge cases ───
results["phase5"] = run_phase(
    "Hardening — Edge Cases & Error Handling",
    """Harden the application:

1. Add proper error handling to Router.php — return 404 JSON for unknown routes, 405 for wrong HTTP methods

2. Add request size limit to the front controller — reject requests with body > 1MB

3. Add rate limiting stub in src/Middleware/RateLimit.php (in-memory counter, 100 req/min per IP)

4. Fix any SQL injection vectors — make sure ALL queries use parameterized statements

5. Add tests/test_edge_cases.py:
   - Test 404 on unknown routes
   - Test 405 on wrong methods (e.g. DELETE /projects without ID)
   - Test very long strings (10000 chars) in all text fields
   - Test unicode/emoji in task titles and comments
   - Test concurrent-ish operations (create + delete same entity)
   - Test empty request body on POST
   - Test malformed JSON

6. Run the FULL test suite and show results."""
)

# ─── Final Report ───
print(f"\n{'='*70}")
print("STRESS TEST RESULTS")
print(f"{'='*70}")

n_files, files = count_files()
all_pass = True
for phase, passed in results.items():
    status = "PASS" if passed else "FAIL"
    if not passed: all_pass = False
    print(f"  {phase}: {status}")

print(f"\n  Total files created: {n_files}")
print(f"  Overall: {'ALL PASSED' if all_pass else 'FAILURES DETECTED'}")

# Run final test count if pytest exists
r = subprocess.run(
    ["bash", "-c", "cd ~/Developer/fcn-testbed && source venv/bin/activate && pytest tests/ --tb=no -q 2>&1 | tail -5"],
    capture_output=True, text=True
)
print(f"\n  Final pytest run:\n{r.stdout}")
