# Working in Cataclysm: Signal

This is theomgdev's fork of Cataclysm-DDA. LLM help is welcome here, unlike
upstream, and this file says what "welcome" actually means so that neither the
people nor the models working here have to guess. The fork is named for its
first rule.

## Rule one: maximise the value to garbage ratio

Everything else in this file is a consequence of this one. Every change you make
carries some value and some garbage, and your job is to push that ratio as high
as it will go — not to produce more, faster.

The concrete form of it, the part you can actually measure: add up everything
you write for a change — the commit message, the pull request body, the comments
left in the source, any markdown you touch — and it has to come out shorter than
the code that change contains. Two lines of code do not get twenty lines of
explanation. When the writing is longer than the thing it describes, cut the
writing.

## What garbage means

Research on this settled on three properties, and they describe the problem
better than any list of symptoms. Slop has *superficial competence*: consistent
naming, tests that exist, documentation that is present, a clean diff — and it
is still wrong or pointless underneath. It has *asymmetric effort*: seconds to
generate, an hour to review, with the cost pushed onto whoever reads it. And it
is *mass producible*, which is why maintainers drown rather than merely
disagree. An agent can open six pull requests in a day; nobody can review six.

That abstraction has very concrete forms, and they are what get contributions
rejected.

In code, the dangerous case is not the hallucinated API call or the
out-of-scope variable — CI catches those cheaply. It is code that compiles,
passes every test, and is quietly wrong. Next to it sit the abstraction layer
built for a problem that needed ten lines, the duplicated block that should have
been a call to something that already exists, the unused helper left behind, and
the invented naming convention. Measurement backs this up: across 211 million
lines, duplicated blocks grew several times over, refactoring collapsed, and
AI-heavy code churned far more than the code around it.

In comments, the most common complaint by a distance is that there are simply
too many of them, and that they answer "what" when the line below already says
what. A comment that restates its own line is garbage by definition — it costs a
reader time and returns nothing. Comment the why, the constraint, the thing that
would surprise someone: why this cache is cleared here, why this order matters.
Heavy commenting usually signals that the writer did not understand the code,
which is exactly the impression you do not want to leave.

In commit messages, the two failures are the bloated register — "In this commit,
improvements were made to the authentication module" — and restating the diff
instead of explaining why the diff exists. The reader can see what changed. They
cannot see what was wrong.

In pull requests, the complaint maintainers repeat most often is that the person
who opened it cannot explain it when asked. Behind that come descriptions
padded out with verbosity that says nothing, invented details stated as fact,
and claims of testing that never happened.

## What follows from that

Do not claim a result you did not observe. If you did not run the test, do not
say it passed; if you ran it, say how many assertions passed rather than that
"all tests pass". Numbers beat adjectives, and `src/visitable.cpp:166` beats
"the visitable implementation".

Passing tests are not proof of correctness. They are proof that nothing you
already thought of is broken. Read the change again for the case nobody wrote a
test for.

Be able to defend it without the model. If you could not answer a reviewer's
question about why a line is there, it is not ready, whoever typed it. This is
the single rule that separates useful assistance from slop.

Finish one thing before starting the next. Volume is the whole reason this
became a crisis — curl closed a six year old bug bounty, Ghostty went zero
tolerance, tldraw started auto-closing outside pull requests, and Jazzband shut
down entirely, all over being flooded. Do not flood anyone, including this fork.

Write like a person, not like a report. Open with a short plain sentence saying
what the change is and why, then keep going in ordinary sentences: what was
wrong, what you did, and anything a reviewer would trip over. No blog structure,
no bulleted lists where a paragraph works, no headline subheadings inside a
commit message, no tables for three items. If a maintainer has to skim past
formatting to reach the content, the formatting lost.

## Do not send this fork's LLM work upstream

CleverRaven bans LLM-sourced code, configuration, issue and PR text, research
and testing results under Licensing and Authorship in `CONTRIBUTING.md`, and
`.github/copilot-instructions.md` is a flat refusal directive. This was tested on
2026-08-29: pull requests #88455 and #88456 were opened upstream and a maintainer
closed both within about two minutes with "AI-generated code is not welcome
here, and never will be". So work freely here, never push LLM-authored work to
CleverRaven, and never present it as human-authored. If something here is
genuinely worth upstreaming, a human writes it there from their own
understanding, or it stays in this fork. Living here is fine; this is not a
staging area.

## What the performance work taught

The fix that made the game fast was not clever. The crafting menu had become
unusable near large item piles because `has_provider_quality` was never declared
virtual, so an `inventory` passed as a `read_only_visitable` never reached its
own cached, stack-aware implementation and fell through to a generic one that
walked every item through two `std::function` indirections and remembered
nothing. Making it virtual and mirroring the override already sitting next to it
turned tens of seconds into nothing noticeable.

That shape repeats, so look for it. The wins all came from deleting work that did
not need doing: containers returned by value that should have been references,
the same inventory query repeated inside a sort predicate, a temporary item
constructed for every string comparison. Prefer that to cleverness, and prefer
matching a cache that already exists to inventing new invalidation rules. One
confident guess about the cause was wrong here before the evidence corrected it,
and what made the final diagnosis trustworthy was that upstream issue #88351
carried a flamegraph pointing independently at the same function.

Finally, keep the game portable. It is single-threaded by design and runs on
everything from a phone to a desktop, and upstream rejects multithreading
outright in `doc/FREQUENTLY_MADE_SUGGESTIONS.md`. Performance work here means
removing waste, which helps every machine by the same proportion, not adding
parallelism that helps one and breaks others.
