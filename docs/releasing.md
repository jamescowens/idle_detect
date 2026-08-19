# Release procedure

Cutting a release touches four systems that do not talk to each other: the git
repo, GitHub Actions, the GitHub release page, and OBS. Each has its own idea of
what the package contains, and they drift silently. This document is the
checklist that keeps them in step.

Every trap listed under [Traps](#traps) has actually happened. Do not skip steps
because they look pedantic.

## 1. Before you tag

### 1.1 Version fields

Three places carry the version, and the `.deb` version does **not** come from the
tag:

| file | field | consumed by |
|---|---|---|
| `release.h` | `IDLE_DETECT_VERSION_*`, `g_version_datetime` | the binaries |
| `debian/changelog` | top entry `idle-detect (X.Y.Z.W-1)` | `dpkg-parsechangelog`, the `.deb` |
| OBS `idle_detect.spec` | `Version:` | set automatically by the `set_version` service |

```bash
grep -E "VERSION_(MAJOR|MINOR|PATCH)|g_version_datetime" release.h
dpkg-parsechangelog -l debian/changelog -S Version
```

Set `g_version_datetime` to the date you actually expect to tag on.

### 1.2 Packaging file-set audit — do not skip this

**This is the check that would have prevented the 0.9.2.0 OBS breakage.** The RPM
spec lists every installed file explicitly. Adding or removing an installed file
without updating it fails every RPM target, while Arch and Debian pass, because
their packaging derives the file list from `cmake --install`.

```bash
SPEC="$HOME/builds/obs/home:jamescowens/idle_detect/idle_detect.spec"
STAGE=$(mktemp -d)

cmake -S . -B build/rel -DCMAKE_BUILD_TYPE=Release
cmake --build build/rel -j$(nproc)          # ALL targets, not just one
DESTDIR="$STAGE" cmake --install build/rel

find "$STAGE" -path '*/bin/*' -maxdepth 4 -type f -printf '%f\n' | sort > /tmp/installed.txt
sed -n 's|^%{_bindir}/||p' "$SPEC" | sort > /tmp/spec.txt

comm -13 /tmp/spec.txt /tmp/installed.txt   # installed but NOT packaged -> RPM "unpackaged files"
comm -23 /tmp/spec.txt /tmp/installed.txt   # packaged but NOT installed -> RPM "File not found"
```

Both lists must be empty. Check the same way for anything outside `%{_bindir}`
that you added: unit files, presets, config templates, `%{_datadir}` content.

### 1.3 Tests and CI

```bash
cmake --build build/rel -j$(nproc) --target idle_detect_tests
./build/rel/idle_detect_tests
```

Then confirm the CMake workflow is green **on the exact commit you intend to
tag**, not merely on the branch:

```bash
gh run list --branch development --limit 1 --json conclusion,headSha
git rev-parse --short HEAD
```

Note the gap: the `.deb` workflow only runs on tag pushes, so a packaging or
architecture-specific failure cannot be seen before tagging. 32-bit ARM in
particular has caught bugs the 64-bit builders never see. Either accept that the
first tag may need to be re-cut, or run the workflow manually on a scratch tag
first.

### 1.4 Documentation

- `RELEASE_NOTES_X.Y.Z.W.md` written (untracked by convention; it becomes the
  release body).
- `docs/testing.md` updated with what was actually tested for this release, and
  what was not.
- README and `docs/` reflect any behavior change. A feature nobody documented is
  a feature nobody can use.

## 2. Cut the tag

Releases live on `master`; development happens on `development`.

```bash
git checkout master
git merge --no-ff development -m "Merge branch 'development'"
git tag -a X.Y.Z.W -m "X.Y.Z.W

Correctness release.

Highlights:
- ..."
git push origin master
git push origin X.Y.Z.W          # never 'git push --tags'
```

Use an **annotated** tag: `tar_scm` resolves `@PARENT_TAG@` with `git describe`,
which prefers annotated tags. Push the single tag by name; `--tags` will try to
clobber unrelated old tags.

### If you must move a tag

Acceptable only while nothing has consumed it — no published release, no
distributed artifacts:

```bash
git tag -l X.Y.Z.W --format='%(contents)' > /tmp/tagmsg.txt   # keep the annotation
git tag -d X.Y.Z.W
git push origin :refs/tags/X.Y.Z.W
git tag -a X.Y.Z.W -F /tmp/tagmsg.txt <new-commit>
git push origin X.Y.Z.W
```

Then **force-refresh every consumer's cached clone** (see [4.1](#41-advance-the-tar_scm-clone-first)),
because they will otherwise keep the deleted tag object.

## 3. GitHub release

The `Linux Packages (.deb)` workflow runs on the tag, builds 15 packages
(bookworm/jammy/noble/plucky/trixie × amd64/arm64/armhf), and creates a **draft**
release with the `.deb`s, the dbgsym packages, and `SHA256SUMS.txt`.

Do not create the release manually — you will collide with the workflow. Wait for
it, then fill in the body:

```bash
gh run list --limit 3                       # wait for "Linux Packages (.deb)" to finish
gh release view X.Y.Z.W --json isDraft,assets --jq '{draft:.isDraft, assets:(.assets|length)}'
gh release edit X.Y.Z.W --notes-file RELEASE_NOTES_X.Y.Z.W.md
```

Expect 22 assets: 15 runtime `.deb`, 6 dbgsym, 1 `SHA256SUMS.txt`.

Review, then publish:

```bash
gh release edit X.Y.Z.W --draft=false --latest
```

The release page's **Contributors** block lists the accounts `@mentioned in the
release body`, not the commit authors. Mention the people you want credited.

## 4. OBS

```bash
cd ~/builds/obs/home:jamescowens/idle_detect
```

### 4.1 Advance the tar_scm clone first

`tar_scm` keeps a working clone at `./idle_detect/` inside the package directory
and reuses it. `@PARENT_TAG@` is `git describe` of that clone's **HEAD**, so a
stale HEAD silently rebuilds the previous release:

```
merge: origin/0.9.1.0 - not something we can merge
Identical target file idle_detect-0.9.1.0.tar.gz already exists, skipping..
```

Advance it before running anything:

```bash
git -C idle_detect fetch --tags --force --prune origin
git -C idle_detect reset --hard origin/master
git -C idle_detect describe --tags        # MUST print the new version
```

### 4.2 Regenerate and commit

```bash
osc service runall
osc status                # expect: new tarball as '?', old one still tracked
grep -E "^Version:" idle_detect.spec
head -1 debian.changelog

osc add idle_detect-X.Y.Z.W.tar.gz
osc rm --force idle_detect-<previous>.tar.gz
osc status                # confirm ONLY the intended changes
osc commit -m "Update to X.Y.Z.W"
```

Use explicit `osc add` / `osc rm`. **Never `osc addremove`** — it will sweep the
`idle_detect/` clone directory into the package.

### 4.3 Watch the results

```bash
osc results               # must be run from the package directory
osc buildlog <repo> <arch>
```

Two repositories, `Fedora_Rawhide/x86_64` and `Fedora_38/armv7l`, have been
`broken` for a while and are unrelated to any given release. Everything else
should reach `succeeded`.

## 5. Post-release verification

- Package upgrade on a machine running the previous version; confirm the service
  is active afterwards.
- **Reboot it.** Boot start is the failure mode that hides best; the service can
  look perfectly healthy and never start at boot.
- `read_shmem_timestamps` reports the new version.
- On a BOINC machine, confirm the segment is still mapped.

## Traps

Each of these cost real time. They are listed with the evidence so they are
recognizable next time.

**The RPM spec is not in this repo.** It lives in the OBS package. Changing the
installed file set here breaks it there, with no signal until OBS builds. 0.9.2.0
retired `idle_detect_wrapper.sh` and added `dc_fah_v8` and
`boinc_selinux_shmem_policy.sh`; every RPM target failed with
`File not found: /usr/bin/idle_detect_wrapper.sh` while Arch passed. See
[1.2](#12-packaging-file-set-audit--do-not-skip-this).

**`tar_scm` reuses a clone whose HEAD it never advances.** See
[4.1](#41-advance-the-tar_scm-clone-first).

**The `.deb` workflow only runs on a tag.** 0.9.2.0's first tag failed all five
armhf jobs on a test that compared pointer identity — the allocator reused the
freed address, which 64-bit builders never hit. The tag had to be moved.

**Never assert pointer identity in tests.** A freed address is commonly reused,
reliably so on 32-bit.

**`osc results` needs the package directory.** Run elsewhere it reports a
confusing "Git SCM package working copy" error that has nothing to do with the
problem.

**Contributors on the release page come from `@mentions`,** not commits. The repo
Insights graph is separate, is commit-derived, is default-branch only, and its
`stats/contributors` endpoint returns `202` while GitHub recomputes it.
