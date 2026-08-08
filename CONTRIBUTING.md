# Contributing to Koi-DOS

Patches are welcome. This page says what happens to your code when you send it,
in plain words first and with the exact wording afterwards, because a term that
only appears in a licence file is a term people find out about too late.

## The short version

**You keep the copyright in what you write.** Nothing here transfers ownership,
and you remain free to use, publish, sell, or relicense your own work anywhere
else, however you like.

**Koi-DOS may one day be sold under a separate commercial licence.** By sending
a contribution you give the Maintainer permission to include your code in such
a licence — as part of Koi-DOS, never as a piece sold on its own.

**You are not paid for that.** There is no revenue share. If a commercial
licence is ever issued, the money is not split, and you should know that before
you send code rather than after.

Why it works this way: the alternative is that a project with twenty
contributors can never be licensed at all, because doing so means finding
twenty people, some of whom stopped answering email in a year nobody remembers.
Deciding it now, while it is one person's project and costs nobody anything, is
the honest moment to decide it.

If that is not acceptable to you, say so — see [Contributions without commercial
rights](#contributions-without-commercial-rights) below. It is a real option and
not a polite fiction.

## Signing off

Put these two lines at the end of the commit message of every commit you send:

```
Koi-DOS-Contribution: I grant the rights in Section 10 of KNCL, and I have the right to grant them.
Signed-off-by: Your Name <your@email>
```

`git commit -s` writes the second line for you.

The first line is the one that matters here. The licence says that submitting a
contribution grants these rights; the sign-off is you saying so in your own
words, in a record that stays attached to the code. Contributions without it may
be asked for one before being merged.

By signing off you are stating that:

- you wrote the contribution, or otherwise have the right to submit it;
- nobody else — an employer, a client, a university — holds rights that would
  prevent you granting the licences in Section 10;
- as far as you know it does not infringe anyone's copyright, patent, or trade
  secret; and
- any third-party material in it is identified and comes with its licence.

That last one is not a formality. Third-party code has to be listed in
[LICENSE-MANIFEST](LICENSE-MANIFEST) with the licence that actually applies to
it, and "third-party" on its own is not an answer to "may I use this".

## Contributions without commercial rights

If you want to contribute but do not want your code included in a commercial
release, say so in the pull request. It can be accepted as
`KNCL-NONCOMMERCIAL-ONLY` (Section 10.8): it ships with Koi-DOS like anything
else, it is listed by name in `LICENSE-MANIFEST`, and any commercial release has
to build without it.

This needs agreement in writing before merging, and it will not be agreed to for
something the system cannot run without — a driver everything depends on cannot
be the one file a release has to be built without. For a self-contained piece it
is straightforward.

Asking for this is not held against you.

## The exact terms

The short version above is a summary and is not the licence. Where the two
differ, [LICENSE](LICENSE) controls — Section 10 in particular, and Section 1 for
what the words mean.

## Practical notes

**Build it first.** `make` must finish with no warnings; `-Werror` is on, and it
is on deliberately.

```
make            # bootloader, kernel, programs, SDK
make check      # no undefined symbols, no relocations
./qemu.sh       # boot it
```

**Say why, not what.** The comments in this tree explain the reason a thing is
done the way it is — which register lies, which sequence the hardware insists
on, what was tried first and how it failed. A comment that restates the code is
noise; a comment recording the evening that produced the code is the most
valuable thing in the file. Match what is around you.

**English in the repository.** Code, comments, commit messages and documentation
are in English, whatever language the discussion happened in.

**No disk images, no game data, no binaries.** `.img`, `.fd`, `.wad` and build
output do not belong in git, and a WAD file must never be committed at all.

**Say what you tested it on.** "Works in QEMU" and "works on a ThinkPad T430"
are different claims, and for a driver the second one is the one that counts.
This system exists to run on real machines, and hardware disagrees with
emulators in ways that only turn up on hardware.
