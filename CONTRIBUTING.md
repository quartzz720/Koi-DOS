# Contributing to Koi-DOS

Patches are welcome. This page says what happens to your code when you send it.
It is short now, and it used to be long, and the difference is the whole point.

## The short version

**Koi-DOS is MIT.** Send a patch and it goes in under MIT, like everything else.

**You keep the copyright in what you write.** Nothing here transfers ownership.
You remain free to use, publish, sell or relicense your own work anywhere else,
however you like.

**There is nothing else to agree to.** No commercial-licensing grant, no
revenue-share question to answer, no clause about what happens if the project is
ever sold. MIT already lets anybody do all of that, including you, including
somebody neither of us has met.

This page used to explain a source-available licence that forbade commercial use
and reserved to the maintainer the right to license the whole work commercially.
That machinery existed so the licence could be changed later without having to
find twenty contributors, some of whom stopped answering email in a year nobody
remembers. It was used for exactly that, once, to move to MIT. Then it was no
longer needed.

## Signing off

Put this line at the end of the commit message of every commit you send:

```
Signed-off-by: Your Name <your@email>
```

`git commit -s` writes it for you.

It is the Developer Certificate of Origin sign-off: you are saying the code is
yours to give, or that you got it from somewhere that allowed you to pass it on
under these terms. It is a record attached to the code rather than a promise in
a thread, which is the only reason it is asked for.

## What makes a patch easy to take

**One thing per commit**, with a message that says what changed and why. The why
is the part that cannot be recovered from the diff a year later.

**Comments that explain a decision, not the code.** `/* increment the counter */`
next to `counter++` helps nobody. `/* counted rather than sampled: a click lasts
a tenth of a second and a poll thirty times a second will sooner or later look
between the press and the release */` is the reason the code is shaped that way,
and is worth more than the code.

**Say what you tested and how.** "Works on my machine" and "boots in QEMU" are
different claims. If something is untested, say that instead — a known gap is
useful and a wrong claim is not.

**English in the repository.** Code, comments, commit messages and documents.
Discussion elsewhere can be in whatever language suits.

## Third-party material

If a patch brings in code or data somebody else wrote, say so, name its licence,
and add it to [LICENSE-MANIFEST](LICENSE-MANIFEST) with its licence text under
`third-party/`. A permissive licence is not a reason to skip this: MIT, BSD,
Apache-2.0 and OFL all require the notice to be kept, and dropping a notice is
the one way to get a permissive licence wrong.

GPL code cannot go in. Not because of anything about the GPL, but because this
is MIT and the two cannot both be true of one file.

## If something here is wrong

Say so. This document has been rewritten once already because the licence under
it changed, and a document that describes terms that no longer apply is worse
than no document.
