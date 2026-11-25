# Changelog

## 1.0.0 - 2026-11-25

The first release.

- **Finds** an application's bundle, caches, containers, preferences, launch agents and daemons, privileged helpers, logs and package payload, and shows why each item matched and how certain the match is.
- **Reads package receipts** with its own binary `.bom` parser, checked path for path against `lsbom` on 139 real receipts (614,347 paths, 0 differences).
- **Leaves alone** what belongs to other applications: sibling identifiers, LaunchServices owners, shared vendor folders (listed unchecked) and locations protected by SIP.
- **Removes** to the Trash by default, or permanently with a separate acknowledgement. Dry Run touches nothing. Every run writes a JSON audit report.
- **Try it safely** on a synthetic disk with `--root`.

Known limits:

- Ad-hoc signed and not notarized, so macOS blocks a downloaded copy until you allow it.
- The download is for Apple silicon. On an Intel Mac, build from source (not tested).
- Administrator removal is implemented but has not been exercised end to end. The interface has only been run on macOS 27, and the tests on macOS 26 and 27.
