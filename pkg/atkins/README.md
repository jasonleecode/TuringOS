# Atkins

Atkins provides tools for testing L4Re applications.

libgtest and libgmock are provided as the basic test libraries. They can be
used in both host and target mode, making it possible to write both functional
tests and basic target-independent unit tests.  The host versions are currently
not built but can be enabled in `.gmock/Makefile` and `.gtest/Makefile`,
respectively.

In addition, the `./atkins` directory contains utilities and fixtures
specifically for the L4Re environment.

# Documentation

This package is part of the L4Re Operating System Framework. For
documentation and build instructions please refer to
[l4re.org](https://l4re.org).

# Contributions

We welcome contributions. Please see the
[contributors guide](https://l4re.org/contributing/).

# License

Detailed licensing and copyright information can be found in the
[LICENSE](LICENSE.spdx) file.
