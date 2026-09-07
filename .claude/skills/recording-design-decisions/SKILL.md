---
name: recording-design-decisions
description: Use when brainstorming or designing with the owner and a question has 2+ answers that would lead to different work — before asking it, and again the moment the owner answers. Also use when a spec, plan, or review says "owner chose", "đã chốt", "decided", or when someone asks "why did we pick X" and the answer lives only in an old chat.
---

# Recording design decisions

## Overview

Every fork asked during brainstorming is written to one file per topic,
**as it happens**, in a fixed shape: the question, every option offered
(with the recommended one marked), and the owner's pick. A decision that
lives only in chat is lost at the next context reset, and the next session
re-asks it or, worse, silently picks differently.

Baseline failure this skill exists for (observed 2026-09-06, lane G):
three questions asked and answered, spec started, nothing written down
until the owner demanded it. The record is not an end-of-session summary.
It is written per question.

## When to use

- The brainstorming skill is running and you are about to ask a question
  with options (multiple choice or "A vs B").
- The owner answers such a question, or nods through a number you chose
  ("ok, tiếp") — that is a decision too.
- You are writing a spec section "Quyết định đã chốt" — it must cite the
  record, not restate chat.
- Someone asks why a choice was made.

Not for: facts derivable from code or git, implementation details the
plan decides, or reviewer findings (those go in the spec's §8).

## The record

Path: `docs/superpowers/decisions/<YYYY-MM-DD>-<topic>.md`, one file per
lane/feature, created at the FIRST question. Link it from the spec header.

Each question is one section in this exact shape:

```markdown
## Q<n> — <the question as asked, one line>

| # | Phương án | |
|---|---|---|
| 1 | <option 1, one line> | khuyên dùng |
| 2 | <option 2> | |
| 3 | <option 3> | |

**Chọn: <n>.** <owner's extra words if any, verbatim>
```

REQUIRED per section: the question line, every option that was offered
(none dropped, none added after the fact), the "khuyên dùng" mark on the
one you recommended, and the `**Chọn:**` line. A section without a
`**Chọn:**` line is an open question, and the file must say so at the top.

Numbers you chose yourself and the owner accepted in passing get their own
Q section titled "<n> số Fable tự chốt" with `**Chọn: giữ**`.

File header: topic, date, who asked, who decides, link to the spec. A
"Bối cảnh nêu trước khi hỏi" section holds the code facts you presented
before Q1, so a reader knows what the owner knew when choosing.

## Order of operations

1. Write the Q section with options **before** sending the question.
2. Owner answers → fill `**Chọn:**` **before** asking the next question.
3. Spec's "Quyết định đã chốt" cites `Q<n>`; plan and reviewers cite the
   same numbers. Reversal later = new section "Q<n> (lật lại <date>)",
   never an edit of the old pick.
4. Commit the record with the spec (docs-only path).

## Quick reference

| Situation | Do |
|---|---|
| Owner says "1" | `**Chọn: 1.**` — then next question |
| Owner says "ok" to a number you proposed | own Q section, `**Chọn: giữ**` |
| Owner adds a condition ("kèm theo…") | quote it verbatim after `**Chọn:**` |
| Reviewer disputes a pick | spec §8 + new Q section if reversed |
| Question had no options (open-ended) | still a section; options table = the answer's alternatives you named |

## Common mistakes

- Writing the record at the end from memory: options get reworded to fit
  the pick. Write before asking.
- Recording only the chosen option: the value of the record is the road
  not taken.
- Putting the decision in the spec only: the spec gets rewritten; the
  record is append-only.
