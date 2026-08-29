# Working in this fork

This is theomgdev's fork of Cataclysm-DDA. LLM help is welcome here, unlike
upstream, and this file says what "welcome" actually means so that neither the
people nor the models working here have to guess.

The rule that matters most is the ratio. Add up everything you write for a
change — the commit message, the pull request body, the comments left in the
source, any markdown you touch — and it has to come out shorter than the code
that change contains. Two lines of code do not get twenty lines of explanation.
This is the whole reason LLM contributions earned a bad name: a model will
happily produce three paragraphs of summary, a bulleted list of key changes, a
table of touched files and a closing note on future considerations for a patch
that moved one `&`. That is padding, and it buries the two sentences that
mattered. When the writing is longer than the thing it describes, cut the
writing.

Write like a person, not like a report. Open with a short plain sentence saying
what the change is and why, then keep going in ordinary sentences: what was
wrong, what you did, and anything a reviewer would trip over. No blog structure,
no bulleted lists where a paragraph works, no headline subheadings inside a
commit message, no tables for three items. If a maintainer has to skim past
formatting to reach the content, the formatting lost. Reference real things —
`src/visitable.cpp:166` beats "the visitable implementation" — and prefer
numbers to adjectives, so say how many assertions passed rather than that all
tests pass.

Do not send this fork's LLM work upstream. CleverRaven bans LLM-sourced code,
configuration, issue and PR text, research and testing results under Licensing
and Authorship in `CONTRIBUTING.md`, and `.github/copilot-instructions.md` is a
flat refusal directive. This was tested on 2026-08-29: pull requests #88455 and
#88456 were opened upstream and a maintainer closed both within about two
minutes with "AI-generated code is not welcome here, and never will be". So work
freely here, never push LLM-authored work to CleverRaven, and never present it
as human-authored. If something here is genuinely worth upstreaming, a human
writes it there from their own understanding, or it stays in this fork. Living
here is fine; this is not a staging area.

That episode taught something past the policy, worth keeping. The fix that made
the game fast was not clever. The crafting menu had become unusable near large
item piles because `has_provider_quality` was never declared virtual, so an
`inventory` passed as a `read_only_visitable` never reached its own cached,
stack-aware implementation and fell through to a generic one that walked every
item through two `std::function` indirections and remembered nothing. Making it
virtual and mirroring the override already sitting next to it turned tens of
seconds into nothing noticeable.

That shape repeats, so look for it. The wins all came from deleting work that
did not need doing: containers returned by value that should have been
references, the same inventory query repeated inside a sort predicate, a
temporary item constructed for every string comparison. Prefer that to
cleverness, and prefer matching a cache that already exists to inventing new
invalidation rules. Verify before claiming, too — one confident guess about the
cause was wrong here before the evidence corrected it, and what made the final
diagnosis trustworthy was that upstream issue #88351 carried a flamegraph
pointing independently at the same function.

Finally, keep the game portable. It is single-threaded by design and runs on
everything from a phone to a desktop, and upstream rejects multithreading
outright in `doc/FREQUENTLY_MADE_SUGGESTIONS.md`. Performance work here means
removing waste, which helps every machine by the same proportion, not adding
parallelism that helps one and breaks others.
