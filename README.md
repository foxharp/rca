
# _rca_: a rich/RPN calculator

_rca_ is a convenient, non-programmable, command-line calculator, for
typical one-off professional and everyday problems.
It uses the mpdecimal math library for more reliable accuracy
than native floating point.  _rca_ is similar to both the _bc_ and
_dc_ Unix commands, but unlike them, it can seamlessly evaluate both
RPN and infix.  It's less technical, and perhaps less rigorous, than
either, but hopefully it's also more user-accessible.  _rca_ is
written in C, and is maintained in a single source file.

_rca_ is very versatile.  Examples follow, but the
[man page](https://foxharp.github.io/rca/rca-man.html)
and rca's
[help text](https://foxharp.github.io/rca/rca-help.html)
have all the details.

_rca_ is hosted at [github](https://github.com/foxharp/rca).

 - At its heart, _rca_ is a reverse polish notation calculator in the
   HP tradition.

    ```
        34 11 *
                                     374
        sto
        52 13 /
                                       4
        rcl
                                     374
        *
                                   1,496
    ```

 - It seamlessly evaluates infix-style expressions, either mixed with
   RPN input, or in a full-time infix mode (not shown here).
    ```
        ((lastx + pi) * 2)
                                   2,998.28
        (8 - 5) /
                                     999.428
    ```

 - It runs in either floating point or integer mode.
    ```
        D
         Mode is signed decimal (D).  Integer math with 64 bits.
                                     374
                                   1,496
                                     999     # warning: accuracy lost, was 999.4277284357265288
    ```

 - It has the trig and math functions of a scientific calculator.
    ```
        30 sin 2 ^ 30 cos 2 ^ + sqrt
                                       1.000
    ```
    or, using infix:
    ```
        (sqrt (sin(30) ** 2 + cos(30) ** 2))
                                       1.000
    ```
    and with lots of digits:
    ```
        30 digits 0 degrees
         trig functions will now use radians
        pi
                                       3.14159265358979323846264338328
        4 /
                                       0.78539816339744830961566084582
        sin
                                       0.707106781186547524400844362105
        2 ^
                                       0.5
    ```


 - It has the word width selection, bitwise operators, and multiple bases
    of a programmer's calculator.
    ```
        32 bits  H
         Mode is hex (H).  Integer math with 32 bits.
        1 zf
         Zero fill of hex/octal/binary output is now on
        5
                             0x0000,0005
        8 rol
                             0x0000,0500
        0x80010000 |
                             0x8001,0500
        b
                             0b10000000,00000001,00000101,00000000
    ```

 - It supports variables.
    ```
        (_a = 7 + 10; _b = 11 + 43)
                                      54
        (_sum = _a + _b)
                                      71
        variables
                           _a  17
                           _b  54
                         _sum  71
    ```

 - It also has a selection of convenient unit conversions.
    ```
        3.5 i2mm
                                      88.9
        mm2i
                                       3.5
        1 oz2g
                                      28.3495
        2 m2ft
                                       6.56168
        30 mpg2l100km
                                       7.84049
    ```

 - And it can be trivially incorporated into shell scripts.
    ```
        $ source rca_float
        $ foo=$(fe "22 / 7")
        $ echo $foo
        3.143
        $ fc "$foo > 3.1" && echo yes
        yes
    ```
-----------

_rca_'s BSD license can be found in the source, at
https://github.com/foxharp/rca, or viewed with the "license" command.

<!--
# Copyright (c) 2024-2026 Paul Fox <pgf@foxharp.boston.ma.us>
# SPDX-License-Identifier: BSD-2-Clause
-->
