
April 1, 2026

There are currently two branches in the _rca_ repo:

  - The "main" branch came first, and covers releases up through v25.
    The code on "main" uses the host's floating point unit, and the
    libc/libm math libraries.

  - The "mpdecimal" branch (now the default, on github) uses the
    mpdecimal library (libmpdec), from
    [bytereef.org](https://www.bytereef.org/mpdecimal/index.html).

There is an intentional gap in the version numbers between the two
branches.  Versions v25 (libm) and v41 (libmpdec) are the same,
featurewise.

The current plan is that further development will happen on the
mpdecimal branch.

One complication is that somehow libmpdec got dropped from Debian,
for a release or two, so it's also missing from Ubuntu for a release
or two.  It's available on Debian again as of Debian 13 (trixie) as
libmpdec-dev.  It's also on Ubuntu 25, though I'm not sure of the
name.

Since I'm on Ubuntu 24, I've been developing against version 4.0.1,
from https://www.bytereef.org/mpdecimal/download.html.
