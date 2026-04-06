# Interview Spoken Companion Design

## Goal

Create a spoken-answer companion for the KVStore interview Q&A that sounds like a real candidate speaking in a live interview, not like a written document.

## Why A Separate File

The current full interview Q&A is useful as a comprehensive reference, but it is still optimized for coverage.
For practice and memorization, a spoken-answer version should:

- focus on the highest-frequency questions
- use more natural sentence flow
- keep answers technically accurate but easier to say out loud

Keeping this as a separate file avoids losing the value of the full question bank.

## Scope

The spoken companion will:

- live beside the full Q&A under `docs/interview/`
- cover the most important 30 to 40 questions
- include project intro, architecture, durability, Raft, snapshots, cluster-node mode, testing, and boundaries

It will not:

- try to mirror every question from the full file
- replace the full Q&A as the authoritative reference

## Style Rules

- Shorter paragraphs
- More conversational transitions
- Fewer stacked technical nouns per sentence
- Clear distinction between “done now” and “still future work”
- Still defensible under follow-up

## Deliverable

One new file:

- `docs/interview/cpp_infra_kvstore_interview_spoken.md`
