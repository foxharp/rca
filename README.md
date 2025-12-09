
# _rca_ is a rich/RPN (and more) calculator

At it's heart, rca is a command-line reverse polish notation (RPN)
scientific calculator inspired (very loosely) by the H-P calculators. 
But it's far more flexible than that.  Operators include scientific,
logical, bitwise operators, and unit conversions.  The integer width
can be set anywhere between 2 and 64.  In addition, to its native RPN
evaluation, _rca_ can evaluate traditional infix expressions, and
their output falls naturally onto the RPN stack.  Input is free form,
for the most part, and _rca_ is easily incorporated into shell scripts
to provide simple floating point support.

_rca_ is maintained in a single C source file, and requires no
libraries other than -lm (the math library).  It can optionally be
built with readline support.
