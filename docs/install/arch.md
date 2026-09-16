# Arch Linux

The AppImage runs on Arch, but it pays for a mount and a compressed read on
every launch. The Arch package carries exactly the same audited bundle,
extracted once into `/opt/spool`, so the app starts from ordinary files. That is
the only difference between the two; the binaries are identical.

## From the release

Every release publishes `spool-bin-<version>-1-x86_64.pkg.tar.zst` next to the
AppImage. Download it and hand it to pacman:

```sh
sudo pacman -U spool-bin-0.7.13-1-x86_64.pkg.tar.zst
```

Checksums for every asset are in `SHA256SUMS.txt` on the same release.

## From the PKGBUILD

Spool is not on the AUR yet — registration was closed when this was written —
so the recipe lives in this repository instead, and `makepkg` treats it the same
way it would an AUR checkout:

```sh
git clone https://github.com/sachk/spool.git
cd spool/packaging/aur
makepkg -si
```

`makepkg` downloads the portable tarball from the matching GitHub release, so
the clone costs a few megabytes and the build costs nothing — there is no
compiler involved. The release workflow writes the published tarball's checksum
into `PKGBUILD` and regenerates `.SRCINFO` as soon as the assets are up, so the
recipe on `master` verifies what it downloads. Between a version bump and that
release the checksum still belongs to the previous one, and the download it
names does not exist yet either.

Once registration reopens, the same two files go to the AUR unchanged.

## What lands where

`/opt/spool` holds the bundle. `/usr/bin/spool` is a three-line wrapper that
starts it, and the desktop entry, icons and AppStream metadata are installed
where a desktop looks for them. Removing it is `sudo pacman -R spool-bin`;
settings and cache under `~/.config` and `~/.local/share` stay behind.

Qt, mpv and FFmpeg are bundled. The package depends only on the graphics stack
it deliberately does not carry — `libglvnd` and `mesa` — because that is the
part that has to match your driver rather than the build.

Updating means the same command against the next release. The wrapper sets
`SPOOL_PORTABLE_BUNDLE=1`, which is what keeps an installed copy on native
Wayland; the AppImage forces XWayland instead, for reasons that only apply
inside its runtime.
