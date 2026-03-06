
# _rca_: a rich/RPN calculator

_rca_ is simple, yet extremely versatile

 - It's a reverse polish notation calculator in the HP tradition

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
        3 /
                                     999.428
    ```

 - It runs in either floating point or integer mode
    ```
        D
         Mode is signed decimal (D).  Integer math with 64 bits.
                                     374
                                   1,496
                                     999     # warning: accuracy lost, was 999.4277284357265288
    ```

 - It has the trig and math functions of a scientific calculator
    ```
        30 sin 2 ^ 30 cos 2 ^ + sqrt
                                       1.000
    ```
    or
    ```
        (sqrt (sin(30) ** 2 + cos(30) ** 2))
                                       1.000
    ```

 - It has the word width selection, bitwise operators, and multiple bases
    of a programmer's calculator
    ```
        32 bits H
         Mode is hex (H).  Integer math with 32 bits.
        1 zerofill
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

 - It supports variables
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

 - It can be trivially incorporated into shell scripts
    ```
        $ source rca_float
        $ foo=$(fe "22 / 7")
        $ echo $foo
        3.143
        $ fc "$foo > 3.1" && echo yes
        yes
    ```

 - _rca_ is written in C, uses the native floating point library, and
   is maintained in a single source file.  It's not an an arbitrary
   precision calculator like __dc__ or __bc__:  it's a convenient,
   non-programmable, desktop calculator, for typical one-off
   professional and everyday problems.

The
[man page](https://foxharp.github.io/rca/rca-man.html)
and rca's
[help text](https://foxharp.github.io/rca/rca-help.html)
have all the details.

_rca_ is hosted at [github](https://github.com/foxharp/rca).

The license can be found in the source or viewed with _rca_'s
"license" command.  (It's BSD-2-Clause.)
