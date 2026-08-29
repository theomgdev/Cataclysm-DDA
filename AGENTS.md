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
is still wrong or pointless underneath. It
has *asymmetry effort*: it takes vastly less effort to generate than it would
have without AI, while the effort to review it has not moved, so the cost lands
on whoever reads it. And it
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

In tests, the failure is quieter and it is the one that stings a year later.
Generated tests love to pass. They assert too little, they mock away the very
logic they were meant to exercise, and they check that the implementation is
shaped the way it currently happens to be shaped rather than that the behaviour
is right. A test that only notices when you delete the code is not a test; it is
a tripwire around today's implementation, and it will block the refactor you
needed while catching none of the bugs you had.

History belongs in the commit message and the changelog, nowhere else. Code,
README, contributing guides and every other document describe the project as it
is now — not what used to be there, not what you measured on the way, not which
alternative you tried and rejected. The words that mean you are about to leak
your working process into the artifact are "used to", "previously", "it turned
out", "measured on", and "no longer". Design rationale for a constant is
legitimate and welcome; state the reason, not the story that produced it. Say
that a hard edge would make the cap discontinuous, not that both boundaries were
hard until you found otherwise.

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

Assume the happy path is right and go looking at the edges, because that is
where generated code fails. Worth checking specifically: a brand new dependency
pulled in for something trivial, a heavyweight library added to get one
function, errors caught generically or swallowed in silence, names like `data`
and `result` that say nothing, values hardcoded where configuration was meant,
and user input interpolated straight into a query string. None of those break
the build.

Do not over-verify either. The bar is whether a check would change what the
documentation says or what the reader does next. Publishing a predicted number
as though it were measured clears that bar and must be fixed; re-deriving a
figure already measured, or restating a caveat three ways, does not. Paranoia
reads as paranoia, not rigour, and it buries the findings that matter under
qualifications.

Be able to defend it without the model. If you could not answer a reviewer's
question about why a line is there, it is not ready, whoever typed it. This is
the single rule that separates useful assistance from slop.

Keep the change surgical. Every changed line should trace back to what was
asked. Do not improve adjacent code, do not reformat what you happened to scroll
past, do not refactor things that are not broken, and match the surrounding
style even where you would have done it differently. Clean up the imports and
helpers your own change orphaned, and leave pre-existing dead code alone —
mention it instead. A diff full of unrelated tidying is one of the fastest ways
to get a reviewer to stop reading.

Write the least code that solves the problem. No speculative abstraction, no
configurability nobody asked for, no error handling for situations that cannot
occur. If two hundred lines could have been fifty, it should have been fifty.

Research switches settle a question on a branch and then come out. Adding a flag
to A/B two implementations is good practice while it is measuring; the moment it
has answered, delete the losing path and make the winner the only one. What is
left otherwise is API surface, documentation, tests, and a way to be configured
wrong. A parameter earns its place when an expert would genuinely flip it in
production, not because two implementations happen to exist. Efficiency is never
one of those: everyone wants it, so it is not a preference to expose.

Say what you assumed, and stop when you do not know. If a request has two
readings, name both instead of silently picking one; if something is genuinely
unclear, say what is unclear rather than guessing well. Guessing quietly is how
a change ends up looking finished and being wrong.

Do not scatter per-tool instruction files across the repository. One `AGENTS.md`
is the contract, and every assistant reads it. Anything specific to your own
tool — `CLAUDE.md`, `CURSOR.md`, `GEMINI.md`, `.cursor/`, `.claude/` and the
rest — stays local and gitignored. Those files are noise to everyone not using
that tool, they drift out of sync with each other, and a repository that
accumulates one per vendor has already lost the plot.

Say that you had help, and own the result anyway. Most projects that allow
assisted contributions ask for both: around half require the assistance to be
disclosed, and about three quarters require a human in the loop, which is the
same demand from the other side. A line in the pull request, or an `Assisted-by:`
trailer on the commit, costs nothing and tells a reviewer where to look harder.
The trailer is disclosure, not credit: it records that a model helped, while the
author and the accountable party stay human. That distinction has a hard edge in
projects that use the Developer Certificate of Origin — an agent must never add
a `Signed-off-by` line, because only a person can certify it. It also does not
transfer responsibility; the output is yours the moment you open the pull
request. And a model may help you review, but it cannot be the thing
that approves a change; an automated review comment is not a second pair of
eyes, it is the same pair.

If a human cannot review every line, use more than one model rather than none —
one looking for mistakes, one watching this ratio, and something in an advisory
seat by default. Treat what they return as a source to filter, not a checklist
to execute: keep the points that survive contact with the evidence, say plainly
which ones you dropped and why. And keep the limit in view. Independent evidence
is what turns a guess into a finding; a second model agreeing with the first is
still a guess, just a more confident one.

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

## Is this working?

You will know it is when diffs contain only what was asked for, when reviews
stop turning into rewrites, and when questions arrive before the work rather
than after the mistake.

Everything above the upstream section is the general half, and it tracks
[Keel](https://github.com/theomgdev/keel). Fix a general rule there first and
bring it back here, so the two do not drift; anything below that point is this
fork's own and belongs only here.
