# Portability status

FSX keeps filesystem algorithms independent of platform-specific device discovery and inactivity checks. The baseline I/O model uses positional Unix I/O and bounded worker queues; OS-specific geometry/safety code is isolated in the I/O layer.

Target source platforms:

```text
Linux
FreeBSD
DragonFly BSD
OpenBSD
NetBSD
macOS
illumos / other POSIX-like Unix through the generic path
```

The 1.0 release artifacts and full release matrix were built on Linux x86-64. Linux is therefore the only platform described as release-environment verified by this archive.

The source contains native branches for FreeBSD, DragonFly BSD, OpenBSD, NetBSD and macOS device/mount handling. Those branches still require native CI and real-device validation on each OS before destructive operations should be considered production-verified there.

The safety policy is fail-closed: if the platform layer cannot establish a required inactivity invariant, destructive access must return unsupported/safety refusal rather than silently proceeding.
