char *progversion = "v17";
/*
 *
 *	This program is a mediocre but practical stack-based floating
 *	point calculator.  It resembles the UNIX 'dc' command in usage,
 *	but is not as full-featured (no variables or arrays) and is not
 *	infinite precision -- all arithmetic is done as "double".
 *	Commands resembling some of the commands common to HP scientific
 *	calculators have been added as well.
 *		- Paul G. Fox, Wed Dec 29 1993
 *
 *
 *    The calculator has progressed since then:  at the least, I
 *    wouldn't call it mediocre anymore.  I've used it regularly (and
 *    improved it sporadically) over the decades.  It's like a good
 *    pocketknife:  always nearby, with handy features that do what I
 *    need.  Plus, if something's missing, it's usually easy to add.
 *    See the help and the man page for what it can do.  As for the
 *    name?  I needed a new, short name, and "rca" seems unused these
 *    days.  Could do worse for an RPN calculator, but given its age,
 *    and my programming style, maybe the 'r' should stand for "retro".
 *             - pgf, January 2026
 *
 *
 *  If you don't have the Makefile, build with:
 *    doit:         gcc -g -o rca -D USE_READLINE rca.c -lm -lreadline
 *    doit-norl:    gcc -g -o rca rca.c -lm
 *
 */

char *licensetext[] = {
"",
" RCA License                  (SPDX-License-Identifier: BSD-2-Clause) ",
" ------------ ",
" Copyright (C) 1993-2026  Paul Fox ",
" ",
" Redistribution and use in source and binary forms, with or without ",
" modification, are permitted provided that the following conditions ",
" are met: ",
" 1. Redistributions of source code must retain the above copyright ",
"    notice, this list of conditions and the following disclaimer. ",
" 2. Redistributions in binary form must reproduce the above copyright ",
"    notice in the documentation and/or other materials provided with ",
"    the distribution. ",
"",
" This software is provided by the author ``as is'' and any ",
" express or implied warranties, including, but not limited to, the ",
" implied warranties of merchantability and fitness for a particular ",
" purpose, are disclaimed.  In no event shall the author be liable ",
" for any direct, indirect, incidental, special, exemplary, or ",
" consequential damages (including, but not limited to, procurement ",
" of substitute goods or services; loss of use, data, or profits; or ",
" business interruption) however caused and on any theory of liability, ",
" whether in contract, strict liability, or tort (including negligence ",
" or otherwise) arising in any way out of the use of this software, even ",
" if advised of the possibility of such damage. ",
"",
  0 };

/* in addition to progversion, above, Makefile may pass in a
 * definition of CCVERSION */
#ifdef CCVERSION
char *ccprogversion = CCVERSION;
#else
char *ccprogversion = "built " __DATE__ " " __TIME__;
#endif

#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <limits.h>
#include <errno.h>
#include <locale.h>

#ifdef USE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

/* libraries that reportedly support %' for adding commas to %d, %f, etc */
#if defined(__GLIBC__) || defined(__APPLE__)
# define PRINTF_SEPARATORS 1
#else
# define PRINTF_SEPARATORS 0
#endif

/* these are filled in from the locale, if possible, otherwise
 * they'll default to period, comma, and dollar-sign */
char *decimal_pt;   // locale decimal_point
int decimal_pt_len;
char *thousands_sep, *thousands_sep_input;   // locale thousands_sep
char *currency ;   // locale currency_symbol

/* who are we? */
char *progname;

void
usage(void)
{
	fprintf(stderr, "usage: %s [ commands ]\n", progname);
	fprintf(stderr, "  'commands' will be used as initial program input\n");
	fprintf(stderr, "  Use \"%s help\" for documentation.\n", progname);
	exit(1);
}

/* debugging support, runtime controllable */
int tracing;
#define trace(a)  do {if (tracing > 1) printf a ;} while(0)

typedef bool boolean;

#define TRUE 1
#define FALSE 0

typedef int opreturn;

#define GOODOP 1
#define BADOP 0

typedef long double ldouble;

/* global copies of main's argc/argv */
int g_argc;
char **g_argv;

ldouble pi = 3.141592653589793238462643383279502884L;
ldouble e =  2.718281828459045235360287471352662497L;

/* internal representation of operands on the stack.  numbers are always
 * stored as long doubles, even when we're in integer mode.  this could
 * be revisited, but since the FP mantissa is usually as big as the integer
 * word size these days (64 bits), it's probably fine.
 */
struct num {
	ldouble val;
	struct num *next;
};

/* the operand stack */
struct num *stack;
int stack_count;

/* for command repeat, like "sum" */
int stack_mark;

/* for catching infix bugs */
int infix_stacklevel;

/* all user input is either a number or a command operator.
 * this is how operators are looked up, by name
 */
typedef opreturn(*opfunc) (void);

/* operator routines */
typedef struct oper {
	char *name;
	opfunc func;
	char *help;
	int operands;	/* number of operands: used only by infix code */
	int prec;	/* precedence: used only by infix code */
	int assoc;	/* associativity: used only by infix code */
} oper;

/* operator table */
struct oper opers[];

/* tokens are typed -- currently numbers, operators, symbolic, and line-ends */
typedef struct token {
	union {
		ldouble val;   /* NUMERIC: simple value */
		oper *oper;    /* OP or SYMBOLIC points into opers table */
		char *varname; /* VARIABLE: variable's name, malloced */
		char *str;     /* UNKNOWN: points to input buffer, for errors */
	} val;
	int type;
	int imode;	    /* input mode: if NUMERIC, how was it entered?  */
	int alloced;	    /* should this token be freed or not? */
	struct token *next; /* for stacking tokens when infix processing */
} token;

/* values for token type */
#define UNKNOWN -1
#define NUMERIC 0
#define SYMBOLIC 1
#define OP 2
#define EOL 3
#define VARIABLE 4

/* values for # of operands field in opers table:
 *  1 & 2 are used verbatim as operand counts
 *  0 denotes a pseudo-op, i.e. it manipulates the calculator itself
 * -1 (Sym) means a "named number", like pi, or lastx
 * -2 (Auto) is a pseudo-op that wants autoprint, like "pop", "exch", "sum" */
#define Sym	-1
#define Auto	-2


/* 6 major modes:  float, decimal, hex, octal, binary, and raw float.
 * all but float and raw float are integer modes.  (raw float is a
 * debug mode:  it uses the printf %a format)
 */
int mode = 'F';			/* 'F', 'D', 'H', 'O', 'B', 'R' */
boolean floating_mode(int m) { return (m == 'F' || m == 'R'); }

/* if true, exit(4) on error, warning, or access to empty operand stack */
boolean exit_on_error = FALSE;

/* if true, print the top of stack after any line that ends with an operator */
boolean autoprint = TRUE;

/* informative feedback, which is only printed if the command generating
 * it is followed by a newline */
void pending_printf(const char *fmt, ...);
#define p_printf pending_printf    // shorthand


/* if true, will decorate numbers, like "1,333,444".  */
boolean digitseparators = PRINTF_SEPARATORS;

/* float_digits may represent either the total displayed precision, or
 * the number of digits after the decimal, depending on
 * float_specifier.  it will be capped at max_precision, whose true
 * value is calculated at startup.  */
int float_digits = 6;
int max_precision = 18;
char *float_specifier = "automatic"; // or "engineering" or "fixed decimal"

/* zerofill controls whether digits to the left of a value are
 * left blank, or shown as zero.  useful for smaller word widths
 * in hex, octal, and binary modes. */
boolean zerofill = 0;

/* rightalign controls whether, when printing, we line up least
 * significant digits (right) or most significant digits (left).  */
boolean rightalignment = 1;

/* I tried making these alignment columns dynamic, adjusting to the
 * max width of a float, or an integer in whichever particular base,
 * but it added complexity for not a lot of value.  So now there are
 * just two choices, which just fit 64 bit octal and binary output. */
#define ALIGN_COL        32
#define ALIGN_COL_BINARY 71

#define LONGLONG_BITS (sizeof(long long) * 8)

/* these all help limit the word size to anything we want.  */
int max_int_width;
int int_width;
long long int_sign_bit;
long long int_mask;
long long int_max;
long long int_min;

/* this is used to control floating point error */
long double epsilon;

/* we don't allow parsing of floating hex input (e.g., -0x8.0p-63) by
 * default, to avoid confusion.  it's enabled after the first use of
 * floating hex output (with "raw" or "Raw") */
boolean raw_hex_input_ok;

/* if true, perform snapping and rounding of float values */
boolean do_rounding = 1;

/* the most recent top-of-stack */
ldouble lastx;

/* counting state variable used to allow variables to be read/write */
int variable_write_enable;

/* where program input is coming from currently */
static char *input_ptr = NULL;

int parse_tok(char *p, token *t, char **nextp, boolean parsing_rpn);

void
memory_failure(void)
{
	perror("rca: malloc failure");
	exit(3);
}

void
error(const char *fmt, ...)
{
	fflush(stdout);

	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	if (exit_on_error)
		exit(4);
}

void
detect_epsilon(void)
{
	epsilon = 1.0L;

	while ((1.0L + epsilon / 2.0L) > 1.0L)
		epsilon /= 2.0L;

	// round up to significant digit
	max_precision = (int)(-log10l(epsilon)); // 18 for 64-bit mantissa
}


/* try and take care of small floating point detritus, by snapping
 * numbers that are very close to integers and zero, and by rounding
 * to our max precision.
 */
ldouble
tweak_float(ldouble x)
{

	ldouble r, abs_x, tolerance, factor;

	if (!do_rounding)
		return x;

	if (x == 0.0L || x == -0.0)
		return x;

	if (!isfinite(x))
		return x;	/* NaN, +Inf, -Inf pass through */

	abs_x = fabsl(x);

	/* snap to integer */
	/* scale tolerance by magnitude.  20 * epsilon is about 2e-18 */
	tolerance = epsilon * 20L;
	if (abs_x > 1.0L)
		tolerance *= abs_x;

	r = roundl(x);
	if (fabsl(x - r) <= tolerance) {
		if (x != r)
			trace(("snap %La (%.20Lg)\n"
				"   to %La (%.20Lg)\n", x, x, r, r));
		return r;
	}

	/* round to max_precision digits */
	factor = powl(10L, max_precision - ceill(log10l(fabsl(x))));
	r = roundl(x * factor) / factor;
	if (x != r)
		trace(("round %La (%.20Lg)\n"
			"   to %La (%.20Lg)\n", x, x, r, r));

	return r;

}

long long
sign_extend(long long b)
{
	if (int_width == LONGLONG_BITS)
		return b;
	else
		return b | (0 - (b & int_sign_bit));
}

void
push(ldouble n)
{
	struct num *p;

	p = (struct num *)calloc(1, sizeof(struct num));
	if (!p)
		memory_failure();

	if (floating_mode(mode) || !isfinite(n)) {
		p->val = n;
		trace((" pushed %Lg\n", n));
	} else {
		p->val = sign_extend((long long)n & int_mask);
		trace((" pushed masked/extended %lld/0x%llx\n",
			(long long)(p->val), (long long)(p->val)));
	}

	p->next = stack;
	stack = p;
	stack_count++;
}

void
result_push(ldouble n)
{
	if (isfinite(n))
		n = tweak_float(n);
	push(n);
}


boolean
peek(ldouble *f)
{
	if (!stack)
		return FALSE;

	*f = stack->val;
	return TRUE;
}

boolean
pop(ldouble *f)
{
	struct num *p;

	p = stack;
	if (!p) {
		error(" empty stack\n");
		return FALSE;
	}
	*f = p->val;
	stack = p->next;
	trace((" popped  %Lg\n", p->val));
	free(p);
	stack_count--;

	if (stack_count < infix_stacklevel) {
		error("BUG: stack level dropped by %d during infix\n",
				infix_stacklevel - stack_count);
	}

	/* remove a stack mark if we've gone below it */
	if (stack_count < stack_mark)
		stack_mark = 0;

	return TRUE;
}

opreturn
toggler(boolean *control, char *descrip, char *yes, char *no)
{
	ldouble n;

	if (!pop(&n))
		return BADOP;

	if (n != 0 && n != 1) {
		push(n);
		error(" error: toggle commands only take 0/1 as an argument\n");
		return BADOP;
	}

	*control = n;

	p_printf(" %s %s\n", descrip, n ? yes : no);
	return GOODOP;
}

opreturn
enable_errexit(void)
{
	return toggler(&exit_on_error,  "Exiting on errors and warnings ",
		"enabled", "disabled");
}

opreturn
assignment(void)
{
	/* This gets decremented with every RPN token executed.  If a
	 * variable is the very next token, we'll do a write to it
	 * instead of a read. */
	variable_write_enable = 2;
	return GOODOP;
}

boolean
are_finite(ldouble a, ldouble b)
{
	if (isfinite(a) && isfinite(b))
		return 1;

	return 0;
}
opreturn
add(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			if (floating_mode(mode) || !are_finite(a,b)) {
				result_push(a + b);
			} else {
				push((long long)a + (long long)b);
			}
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
subtract(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			if (floating_mode(mode) || !are_finite(a,b)) {
				result_push(a - b);
			} else {
				push((long long)a - (long long)b);
			}
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
multiply(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			if (floating_mode(mode) || !are_finite(a,b)) {
				result_push(a * b);
			} else {
				push((long long)a * (long long)b);
			}
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
divide(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			if (floating_mode(mode) || !are_finite(a,b)) {
				result_push(a / b);
			} else {
				push((long long)a / (long long)b);
			}
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
modulo(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			if (floating_mode(mode) || !are_finite(a,b)) {
				result_push(fmodl(a,b));
			} else {
				push((long long)a % (long long)b);
			}
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

long long
int_pow(long long base, long long exp)
{
	long long result = 1;

	for (long long i = 0; i < exp; i++) {
		result *= base;
	}
	return result;
}

opreturn
y_to_the_x(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			if (floating_mode(mode) || !are_finite(a,b)) {
				result_push(powl(a, b));
			} else {
				push(int_pow((long long)a, (long long)b));
			}
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
e_to_the_x(void)
{
	ldouble a;

	if (pop(&a)) {
		result_push(expl(a));
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

/* This is poorly named.  The goal it to report whether the two
 * arguments are both finite (i.e., useful) to an operation, and if
 * not, to propagate the either nan, or inf, in that order, as the
 * result of the operation.  */
boolean
bothfinite(ldouble a, ldouble b)
{
	if (isfinite(a) && isfinite(b))
		return 1;

	if (isnan(a)) {
		push(a);
		return 0;
	} else if (isnan(b)) {
		push(b);
		return 0;
	}
	if (!isfinite(a)) {
		push(a);
		return 0;
	} else if (!isfinite(b)) {
		push(b);
		return 0;
	}
	return 1;
}

boolean
bitwise_operands_too_big(ldouble a, ldouble b)
{
	if (a < LLONG_MIN || a > LLONG_MAX ||
	    b < LLONG_MIN || b > LLONG_MAX) {
		push(a);
		push(b);
		error(" error: bitwise operand(s) bigger/smaller than LLONG_MAX/MIN\n");
		return 1;
	}
	return 0;
}

boolean
bitwise_operand_too_big(ldouble a)
{
	if (a < LLONG_MIN || a > LLONG_MAX) {
		push(a);
		error(" error: bitwise operand bigger/smaller than LLONG_MAX/MIN\n");
		return 1;
	}
	return 0;
}

opreturn
rshift(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			unsigned long long i;
			long long j;

			if (!bothfinite(a, b))
				return GOODOP;

			if (bitwise_operands_too_big(a, b))
				return BADOP;

			// use unsigned for a logical, not arithmetic, shift
			i = (unsigned long long)a;
			j = (long long)b;
			if (j < 0) {
				error(" error: shift by negative not allowed\n");
				push(a);
				push(b);
				return BADOP;
			} else if (b >= sizeof(a) * CHAR_BIT) {
				push(0);
			} else {
				push(i >> j);
			}
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
lshift(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			long long i, j;

			if (!bothfinite(a, b))
				return GOODOP;

			if (bitwise_operands_too_big(a, b))
				return BADOP;

			i = (long long)a;
			j = (long long)b;
			if (j < 0) {
				error(" error: shift by negative not allowed\n");
				push(a);
				push(b);
				return BADOP;
			} else if (b >= sizeof(a) * CHAR_BIT) {
				push(0);
			} else {
				push(i << j);
			}
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
bitwise_and(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			long long i, j;

			if (!bothfinite(a, b))
				return GOODOP;

			if (bitwise_operands_too_big(a, b))
				return BADOP;

			i = (long long)a;
			j = (long long)b;
			push(i & j);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
bitwise_or(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			long long i, j;

			if (!bothfinite(a, b))
				return GOODOP;

			if (bitwise_operands_too_big(a, b))
				return BADOP;

			i = (long long)a;
			j = (long long)b;
			push(i | j);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
bitwise_xor(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			long long i, j;

			if (!bothfinite(a, b))
				return GOODOP;

			if (bitwise_operands_too_big(a, b))
				return BADOP;

			i = (long long)a;
			j = (long long)b;
			push(i ^ j);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
setbit(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			long long i, j;

			if (!bothfinite(a, b))
				return GOODOP;

			if (bitwise_operands_too_big(a, b))
				return BADOP;

			if (b < 0) {
				error(" error: negative bit number not allowed\n");
				push(a);
				push(b);
				return BADOP;
			}
			i = (long long)a;
			j = (long long)b;
			if (b < sizeof(i) * CHAR_BIT)
				i |= (1LL << j);
			push(i);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
clearbit(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			long long i, j;

			if (!bothfinite(a, b))
				return GOODOP;

			if (bitwise_operands_too_big(a, b))
				return BADOP;

			if (b < 0) {
				error(" error: negative bit number not allowed\n");
				push(a);
				push(b);
				return BADOP;
			}
			i = (long long)a;
			j = (long long)b;
			if (b < sizeof(i) * CHAR_BIT)
				i &= ~(1LL << j);

			push(i);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
bitwise_not(void)
{
	ldouble a;

	if (pop(&a)) {
		if (!isfinite(a)) {
			push(a);
			return GOODOP;
		}

		if (bitwise_operand_too_big(a))
			return BADOP;

		push(~(long long)a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
chsign(void)
{
	ldouble a;

	if (pop(&a)) {
		push(-a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
nop(void)
{
	return GOODOP;
}

opreturn
absolute(void)
{
	ldouble a;

	if (pop(&a)) {
		push((a < 0) ? -a : a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
recip(void)
{
	ldouble a;

	if (pop(&a)) {
		result_push(1.0 / a);
		return GOODOP;
	}
	return BADOP;
}

opreturn
squarert(void)
{
	ldouble a;

	if (pop(&a)) {
		result_push(sqrtl(a));
		return GOODOP;
	}
	return BADOP;
}

opreturn
trig_no_sense(void)
{
	error(" error: trig functions make no sense in integer mode");
	return BADOP;
}

boolean trig_degrees = 1;  // work in degrees by default

opreturn
use_degrees(void)
{
	return toggler(&trig_degrees, "trig functions will now use",
		"degrees", "radians");
}

ldouble
to_degrees(ldouble angle)
{
	return (angle * 180.0L) / pi;
}

ldouble
to_radians(ldouble angle)
{
	return (angle * pi) / 180.0L;
}

ldouble
radians_to_user_angle(ldouble rads)
{
	if (trig_degrees)
		return to_degrees(rads);
	else
		return rads;
}

ldouble
user_angle_to_radians(ldouble u_angle)
{
	if (trig_degrees)
		return to_radians(u_angle);
	else
		return u_angle;
}

ldouble
user_angle_to_degrees(ldouble u_angle)
{
	if (trig_degrees)
		return u_angle;
	else
		return to_degrees(u_angle);
}

opreturn
sine(void)
{
	ldouble a;

	if (!floating_mode(mode))
		return trig_no_sense();

	if (pop(&a)) {
		result_push(sinl(user_angle_to_radians(a)));
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
asine(void)
{
	ldouble a;

	if (!floating_mode(mode))
		return trig_no_sense();

	if (pop(&a)) {
		result_push(radians_to_user_angle(asinl(a)));
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
cosine(void)
{
	ldouble a;

	if (!floating_mode(mode))
		return trig_no_sense();

	if (pop(&a)) {
		result_push(cosl(user_angle_to_radians(a)));
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
acosine(void)
{
	ldouble a;

	if (!floating_mode(mode))
		return trig_no_sense();

	if (pop(&a)) {
		result_push(radians_to_user_angle(acosl(a)));
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
tangent(void)
{
	ldouble a;

	if (!floating_mode(mode))
		return trig_no_sense();

	if (pop(&a)) {
		// tan() goes undefined at +/-90
		if (fmodl(tweak_float(user_angle_to_degrees(a)) - 90, 180) == 0)
			result_push(NAN);
		else
			result_push(tanl(user_angle_to_radians(a)));
		lastx = a;
		return GOODOP;
	}

	return BADOP;
}

opreturn
atangent(void)
{
	ldouble a;

	if (!floating_mode(mode))
		return trig_no_sense();

	if (pop(&a)) {
		result_push(radians_to_user_angle(atanl(a)));
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
atangent2(void)
{
	ldouble a,b;

	if (!floating_mode(mode))
		return trig_no_sense();

	if (pop(&b)) {
		if (pop(&a)) {
			result_push(radians_to_user_angle(atan2l(a,b)));
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
log_worker(int which)
{
	ldouble n, l;

	if (pop(&n)) {
		switch(which) {
		default:  // warning suppression
		case 0: l = logl(n); break;
		case 2: l = log2l(n); break;
		case 10:l = log10l(n); break;
		}
		result_push(l);
		lastx = n;
		return GOODOP;
	}
	return BADOP;
}

opreturn
log_natural(void)
{
	return log_worker(0);
}

opreturn
log_base2(void)
{
	return log_worker(2);
}

opreturn
log_base10(void)
{
	return log_worker(10);
}

opreturn
fraction(void)
{
	ldouble a;

	if (pop(&a)) {
		if (!floating_mode(mode)) {
			push(0);
			return GOODOP;
		}
		if (a > 0)
			result_push(a - floorl(a));
		else
			result_push(a - ceill(a));
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
integer(void)
{
	ldouble a;

	if (pop(&a)) {
		if (!floating_mode(mode)) {
			push(a);
			return GOODOP;
		}
		if (a > 0)
			result_push(floorl(a));
		else
			result_push(ceill(a));
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
logical_and(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			push(a && b);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
logical_or(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			push(a || b);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
is_eq(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			push(a == b);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
is_neq(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			push(a != b);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
is_lt(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			push(a < b);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
is_le(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			push(a <= b);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
is_gt(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			push(a > b);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
is_ge(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			push(a >= b);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
logical_not(void)
{
	ldouble a;

	if (pop(&a)) {
		push((a == 0) ? 1 : 0);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
clear(void)
{
	ldouble scrap;

	if (peek(&lastx))
		while(stack) pop(&scrap);

	return GOODOP;
}

opreturn
rolldown(void)			// aka "pop"
{
	(void)pop(&lastx);
	return GOODOP;
}

opreturn
enter(void)
{
	ldouble a;

	if (pop(&a)) {
		push(a);
		push(a);
		return GOODOP;
	}
	return BADOP;
}

// during an infix evaluation, lastx needs to kept at its pre-infix value.
boolean lastx_is_frozen = 0;
ldouble frozen_lastx;

void
freeze_lastx(void)
{
	if (!lastx_is_frozen) {
		if (!peek(&frozen_lastx))
			frozen_lastx = 0;
		lastx_is_frozen = TRUE;
		infix_stacklevel = stack_count;
	}
}

void
thaw_lastx(void)
{
	if (lastx_is_frozen) {
		lastx_is_frozen = FALSE;
		lastx = frozen_lastx;
		if (stack_count != infix_stacklevel + 1)
			error("BUG: stack changed by %d after infix\n",
				stack_count - infix_stacklevel);
		infix_stacklevel = -1;
	}
}

opreturn
repush(void)			// aka "lastx"
{
	if (lastx_is_frozen)
		push(frozen_lastx);
	else
		push(lastx);
	return GOODOP;
}

opreturn
exchange(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			push(b);
			push(a);
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}


/* descriptor for an open_memstream() FILE pointer */
struct memfile {
	char *bufp;
	size_t sizeloc;
	FILE *fp;
};

void
memfile_open(struct memfile *mfp)
{
	mfp->fp = open_memstream(&mfp->bufp, &mfp->sizeloc);
	if (!mfp->fp) {
		perror("rca: open_memstream failure");
		exit(3);
	}
}

struct memfile pp;

void
pending_clear(void)
{
	if (pp.fp) {
		rewind(pp.fp);
		fputc('\0', pp.fp);
		fseek(pp.fp, -1, SEEK_CUR);
	}
}

void
pending_show(void)
{
	if (pp.fp) {
		fflush(pp.fp);
		printf("%s", pp.bufp);
		pending_clear();
	}
}

void
pending_printf(const char *fmt, ...)
{
	va_list ap;

	if (!pp.fp)
		memfile_open(&pp);

	va_start(ap, fmt);
	vfprintf(pp.fp, fmt, ap);
	va_end(ap);
	// ensure it's always null-terminated
	fputc('\0', pp.fp);
	fseek(pp.fp, -1, SEEK_CUR);
}

struct memfile mp;

void
m_file_start(void)
{
	if (!mp.fp)
		memfile_open(&mp);
	rewind(mp.fp);
}

void
m_file_finish(void)
{
	fputc('\0', mp.fp);
	fflush(mp.fp);

}

char *
putbinary(long long n)
{
	int i;
	int lz = zerofill; // leading_zeros;

	n &= int_mask;

	m_file_start();

	fprintf(mp.fp, " 0b");
	for (i = int_width-1; i >= 0; i--) {
		if (n & (1L << i)) {
			fputc('1', mp.fp);
			lz = 1;
		} else if (lz || i == 0) {
			fputc('0', mp.fp);
		}
		if (i && (i % 8 == 0)) {
			if (digitseparators && lz)
				fputs(thousands_sep, mp.fp);
		}
	}

	m_file_finish();

	return mp.bufp;
}

char *
puthex(long long n)
{
	int i;
	int nibbles = ((int_width + 3) / 4);
	int lz = zerofill; // leading_zeros;

	n &= int_mask;

	m_file_start();

	fprintf(mp.fp," 0x");
	for (i = nibbles-1; i >= 0; i--) {
		int nibble = (n >> (4 * i)) & 0xf;
		if (nibble || lz || i == 0) {
		    fputc("0123456789abcdef"[nibble], mp.fp);
		    lz = 1;
		}
		if (i && (i % 4 == 0)) {
		    if (digitseparators && lz)
			    fputs(thousands_sep, mp.fp);
		}
	}

	m_file_finish();

	return mp.bufp;
}

char *
putoct(long long sn)
{
	int i;
	int triplets = ((int_width + 2) / 3);
	int lz = zerofill; // leading_zeros;
	unsigned long long n = (unsigned long long)sn;

	n &= int_mask;

	m_file_start();

	fprintf(mp.fp," 0o");
	for (i = triplets-1; i >= 0; i--) {
		int triplet = (n >> (3 * i)) & 7;
		if (triplet || lz || i == 0) {
		    fputc("01234567"[triplet], mp.fp);
		    lz = 1;
		}
		if (i && (i % 3 == 0)) {
		    if (digitseparators && lz)
			    fputs(thousands_sep, mp.fp);
		}
	}

	m_file_finish();

	return mp.bufp;
}

char *
putunsigned(unsigned long long uln)
{
	m_file_start();

	fprintf(mp.fp, digitseparators ? " %'llu" : " %llu", uln);

	m_file_finish();

	return mp.bufp;
}

char *
putsigned(long long ln)
{
	m_file_start();

	fprintf(mp.fp, digitseparators ? " %'lld" : " %lld", ln);

	m_file_finish();

	return mp.bufp;
}

boolean
check_int_truncation(ldouble *np, boolean conv)
{
	ldouble n = *np;
	boolean changed = 0;

	if (isnan(n) || !isfinite(n)) {
		return 0;
	} else if (n != sign_extend((long long)n & int_mask)) {
		n = sign_extend((long long)n & int_mask);
		changed = 1;
	}

	if (conv)
		*np = n;

	return changed;
}

void
show_int_truncation(boolean changed, ldouble old_n)
{
	if (!changed) {
		p_printf("\n");
		return;
	}

	pending_show();
	if (floating_mode(mode))
		error("     # warning: display format loses accuracy\n");
	else
		error("     # warning: accuracy lost, was %.*Lg\n", max_precision, old_n);

}

boolean
match_dp(char *p)
{
	return (strncmp(p, decimal_pt, decimal_pt_len) == 0);
}

int
min(int a, int b)
{
	return (a < b) ? a : b;
}


int
convert_eng_format(char *buf)
{
	char *p, *odp, *f, *ep, *dp;
	int exp, nexp, shift;

	/* The printf %e format looks like:
	 *     [-]W.FFFFe[-]EE
	 * There's always just one digit before the decimal (which
	 * might be multibyte, in some locales), and there's always a
	 * sign (+ or -) on the two (or more) digit exponent.  Our job
	 * is to move the decimal point to the right (by moving digits
	 * to the left), in order to match a new exponent that is
	 * always a multiple of three.  */
	p = buf;

	// sign
	if (*p == '+' || *p == '-')
		p++;

	// the single leading digit
	p++;

	// the check for the decimal point
	if (strncmp(p, decimal_pt, decimal_pt_len) != 0)
		return 0;

	odp = p;  // remember where the current decimal point starts
	p += decimal_pt_len;

	f = p;	 // the start of fractional part

	// get past the fractional part
	while (isdigit((unsigned char)*p))
		p++;

	if (*p++ != 'e')
		return 0;

	// get the exponent
	exp = strtol(p, &ep, 10);

	// that should have taken us to the end of the buffer
	if (*ep != '\0')
		return 0;

	// reuse ep to point at the exponent
	ep = p;

	// calculate the new exponent...
	if (exp >= 0)
		nexp = (exp / 3) * 3;
	else
		nexp = ((exp-2) / 3) * 3;

	// ...and how many digits we need to shift (either 1 or 2)
	shift = exp - nexp;

	// move some fractional digits left to where the decimal used to be
	p = odp;
	while (shift--)
		*p++ = *f++;

	// then copy a new decimal point into place
	dp = decimal_pt;
	while (*dp)
		*p++ = *dp++;

	// we don't include exponent if it's "e+00"
	if (nexp == 0) {
		*(ep - 1) = '\0';   // zero the 'e'
		return 1;
	}

	// append the exponent's sign and value
	*ep++ = (nexp < 0) ? '-' : '+';
	sprintf(ep, "%02d", abs(nexp));

	return 1;
}

char *
print_floating(ldouble n, int format)
{
	/* 3 bits per decimal digit is just 21 digits in a long long,
	 * and 256 >>> 21 */
	char buf[256];

	m_file_start();
	fputc(' ', mp.fp);
	if (format == 'R') {

		raw_hex_input_ok = TRUE;

		/* NB:  This printing format is accurate and exact,
		 * but there can be at least 4 different normalizations
		 * (combinations of first digit and exponent) that all
		 * represent the same number.  What is printed may
		 * vary from machine to machine because printf may
		 * canonicalize the mantissa differently.  */

		// 1 digit per 4 bits, and 1 of them is before the decimal
		fprintf(mp.fp, "%.*La\n", (LDBL_MANT_DIG + 3)/4 - 1, n);

	} else if (format == 'F' && float_specifier[0] == 'a') {
		char *printfmt;

		// simple:  "auto" uses %g directly
		if (digitseparators)
			printfmt = "%'.*Lg";
		else
			printfmt = "%.*Lg";

		int fd = (float_digits < 1) ? 1 : float_digits;
		fprintf(mp.fp, printfmt, fd, n);

	} else if (format == 'F' && float_specifier[0] == 'f') {
		char *printfmt;
		char *p;
		int decimals, leadingdigits = 0;

		if (digitseparators)
			printfmt = "%'.*Lf";
		else
			printfmt = "%.*Lf";

		/* The goal is to reduce the number of non-significant
		 * digits shown.  If decimals is set to 6, then 123e12
		 * would show "123,000,000,000,000.000000" which is
		 * showing more than max_precision significant digits.  As
		 * long as some of the excess digits are beyond the
		 * decimal point, we can trim some from that end.  So the
		 * code below gives us "123,000,000,000,000.000".  But that
		 * only works for a while.  If the number gets 1e6 times
		 * bigger, then we have this: "123,000,000,000,000,000,000",
		 * and we're back to showing more than we should.  (At that
		 * point the user should be switching to %g mode.)
		 */
		snprintf(buf, sizeof(buf), printfmt, float_digits, n);

		for (p = buf; *p && !match_dp(p); p++) {
			if (isdigit(*p))
				leadingdigits++;
		}

		// in "0.34", the 0 doesn't count toward significant digits
		if (leadingdigits == 1 && *buf == '0')
			leadingdigits = 0;

		if (p) { /* found a decimal point */

			decimals = min(float_digits, max_precision - leadingdigits);
			if (decimals <= 0) decimals = 0;

			snprintf(buf, sizeof(buf), printfmt, decimals, n);
		}
		fputs(buf, mp.fp);

	} else if (format == 'F' && float_specifier[0] == 'e') { // "eng"

		int fd = (float_digits < 3) ? 3 : float_digits;

		/* %e format is easy to dissect, so we start there.
		 * our float_digits is a count of all the digits.  but
		 * the %e "precision" value is just the number of
		 * decimals.  there's exactly one non-fraction digit,
		 * so subtract 1 to specify the %e fraction length */
		snprintf(buf, sizeof(buf), "%.*Le", fd-1, n);

		/* Do text manipulation to renormalize.  */
		if (!convert_eng_format(buf))
			error(" BUG: parse error in engineering format\n");
		else
			fputs(buf, mp.fp);

	}

	m_file_finish();

	return mp.bufp;
}

int
calc_align(int bpd /* bits/digit */, int dps /* digits/separator */)
{
	(void)dps;

	if (!rightalignment)
		return 0;

#if DYNAMIC_ALIGNMENT
	int digits = (int_width + (bpd - 1)) / bpd;
	int seps = ((digits-1)/ dps) * digitseparators;

	return seps + digits + 3;  /* +3 for prefix:  " 0x" */
#else
	if (bpd == 8)
		return ALIGN_COL_BINARY;
	else
		return ALIGN_COL;
#endif
}

void
print_n(ldouble *np, int format, boolean conv)
{
	ldouble old_n, n;
	long long ln;
	long long mask = int_mask;
	int align;
	boolean changed;

	old_n = n = *np;

	if (floating_mode(format) || !isfinite(n)) {
		char *pf;
		pf = print_floating(n, format);
		align = 0;
		if (rightalignment) {
			char *eos, *dp;
			align = ALIGN_COL;
			dp = strstr(pf, decimal_pt);
			if (dp) {
				eos = pf + strlen(pf);
				align += (int)(eos - dp);
			}
		}
		p_printf("%*s\n", align, pf);
		return;
	}

	/* this is a little messy.  this is the general "print a
	 * number" routine, but because it's called at the deep end of
	 * a recursive loop when printing a stack, it also gets
	 * saddled for doing float to int conversion of values on the
	 * stack.  the conversion needs to happen when switching from
	 * float mode to an integer mode:  values need to be masked
	 * and sign extended (if word length is less than native), and
	 * we need to do it here so we can print a message about the
	 * conversion alongside the converted value.  */

	/* check for integer masking, and optionally modify the original.
	 * we'll do that if we're changing modes, but not if we're just
	 * printing in another mode's format.  */
	changed = check_int_truncation(&n, conv);

	/* mode can be float but we can still print in hex, binary
	 * format, etc */
	switch (format) {
	case 'H':
		ln = (long long)n & mask;
		align = calc_align(4, 4);
		p_printf("%*s", align, puthex(ln));
		break;
	case 'O':
		ln = (long long)n & mask;
		align = calc_align(3, 3);
		p_printf("%*s", align, putoct(ln));
		break;
	case 'B':
		ln = (long long)n & mask;
		align = calc_align(1, 8);
		p_printf("%*s", align, putbinary(ln));
		break;
	case 'U':
		unsigned long long uln;
		/* convert in two steps, to avoid possibly undefined
		 * (by the language) negative double to unsigned conversion */
		ln = (long long)n & mask;
		uln = (unsigned long long)ln;
		/* for decimal, worst case width is like octal's */
		align = calc_align(3, 3);
		p_printf("%*s", align, putunsigned(uln));
		break;
	case 'D':
		ln = (long long)n;
		if (!floating_mode(mode) && int_width != LONGLONG_BITS) {
			/* shenanigans to make pos/neg numbers appear
			 * properly.  our masked/shortened numbers
			 * don't appear as negative to printf, so we
			 * find the reduced-width sign bit, and fake
			 * it.
			 */

			mask = (long long)int_mask & ~int_sign_bit;
			if (ln & int_sign_bit) {	// negative
				ln = -1 * (int_sign_bit - (ln & mask));
			} else {
				ln = ln & mask;
			}
		}
		align = calc_align(3, 3);
		p_printf("%*s", align, putsigned(ln));
		break;
	default:
		error(" bug: default case in print_n()\n");
		return;
	}

	show_int_truncation(changed, old_n);
	if (changed)
		*np = n;

}

void
print_top(int format)
{
	if (stack)
		print_n(&stack->val, format, 0);
}

void
printstack(boolean conv, struct num *s)
{
	if (!s)
		return;

	if (s->next)
		printstack(conv, s->next);

	print_n(&(s->val), mode, conv);
}

opreturn
printall(void)
{
	printstack(0,stack);
	return GOODOP;
}

opreturn
printone(void)
{
	print_top(mode);
	return GOODOP;
}

opreturn
printhex(void)
{
	print_top('H');
	return GOODOP;
}

opreturn
printoct(void)
{
	print_top('O');
	return GOODOP;
}

opreturn
printuns(void)
{
	print_top('U');
	return GOODOP;
}

opreturn
printrawhex(void)
{
	print_top('R');
	return GOODOP;
}

opreturn
printbin(void)
{
	print_top('B');
	return GOODOP;
}

opreturn
printdec(void)
{
	print_top('D');
	return GOODOP;
}

opreturn
printfloat(void)
{
	print_top('F');
	return GOODOP;
}

// worker for printstate()
void
rawprintstack(int n, struct num *s)
{
	if (!s) return;

	if (s->next)
		rawprintstack(n-1, s->next);

	p_printf(" %#20llx   %#20.20Lg    %La%s\n",
		(long long)(s->val), s->val, s->val,
		(n == stack_mark) ? "   <-  mark":"");
}

opreturn
printstate(void)
{
	struct num *s;

	p_printf("\n");
	p_printf(" Current mode is %c\n", mode);
	p_printf("\n");
	p_printf(" In floating mode:\n");
	p_printf("  max precision is %u decimal digits\n", max_precision);
	p_printf("  current float display mode is \"%s\", with %d digits\n",
		float_specifier, float_digits );
	p_printf("  snapping/rounding is %s\n", do_rounding ? "on" : "off");
	p_printf("\n");

	p_printf(" In integer modes:\n");
	p_printf("  width is %d bits\n", int_width);
	p_printf("  mask:     %s", puthex(int_mask));
	p_printf("     sign bit: %s\n", puthex(int_sign_bit));
	p_printf("  min:      %s", puthex(int_min));
	p_printf("     max:      %s\n", puthex(int_max));
	p_printf("\n");

	s = stack;
	p_printf(" Stack, bottom comes first:\n");
	if (!s) {
		p_printf("%16s\n", "<empty>");
	} else {
		p_printf(" %20s   %20s\n",
		    "long long", "long double ('%#20.20Lg' and '%La')");
		p_printf("  bottom of stack\n");
		rawprintstack(stack_count, stack);
		p_printf("  top of stack\n");
	}
	p_printf(" stack count %d, depth of the stack mark is %d\n",
			stack_count, stack_count - stack_mark);

	p_printf("\n");
	p_printf("\n Build-time sizes:\n");
	p_printf("  Native sizes (bits):\n");
	p_printf("   sizeof(long long):\t%lu\n", (unsigned long)(8 * sizeof(long long)));
	p_printf("   LLONG_MIN: %llx, LLONG_MAX: %llx\n", LLONG_MIN, LLONG_MAX);
	p_printf("   sizeof(long double):\t%lu\n", (unsigned long)(8 * sizeof(long double)));
	p_printf("   LDBL_MANT_DIG: %u\n", LDBL_MANT_DIG);
	p_printf("   LDBL_MAX: %.20Lg\n", LDBL_MAX);
	p_printf("   LDBL_EPSILON is %Lg (%La)\n", LDBL_EPSILON, LDBL_EPSILON);
	p_printf("  Calculated:\n");
	p_printf("   detected epsilon is %Lg (%La)\n", epsilon, epsilon);
	p_printf("\n");
	p_printf(" Locale elements (%s):\n", setlocale(LC_NUMERIC, NULL));
	p_printf("  decimal '%s', thousands separator '%s', currency '%s'\n",
		decimal_pt ? decimal_pt : "null",
		thousands_sep ? thousands_sep : "null",
		currency ? currency : "null");

	return GOODOP;
}

static char *
mode2name(void)
{
	switch (mode) {
	case 'D':
		return "signed decimal";
	case 'O':
		return "octal";
	case 'H':
		return "hex";
	case 'B':
		return "binary";
	case 'R':
		return "raw hex float";
	case 'F':
	default: // can't happen.  set it to default
		mode = 'F';
		return "float";
	}
}

void
showmode(void)
{

	p_printf(" Mode is %s (%c). ", mode2name(), mode);

	if (mode == 'F') {
		if (float_specifier[0] == 'f') {
			/* float_digits == 7 gives:  123.4560000  */
			p_printf(" Showing %u digits after the decimal"
				" in %s format.\n",
				float_digits, float_specifier);
		} else {	/* 'g', 'e' */
			/* float_digits == 7 gives:  123.4560  */
			p_printf(" Showing %u digits of total precision"
				" in %s format.\n",
				float_digits, float_specifier);
		}
	} else if (mode == 'R') {
		p_printf(" Showing using floating hexadecimal.\n");
	} else {
		p_printf(" Integer math with %d bits.\n", int_width);
	}

}

opreturn
modeinfo(void)
{
	showmode();
	return GOODOP;
}

opreturn
modehex(void)
{
	mode = 'H';
	showmode();
	printstack(1,stack);
	return GOODOP;
}

opreturn
moderawhex(void)
{
	mode = 'R';
	showmode();
	printstack(1,stack);
	return GOODOP;
}

opreturn
modebin(void)
{
	mode = 'B';
	showmode();
	printstack(1,stack);
	return GOODOP;
}

opreturn
modeoct(void)
{
	mode = 'O';
	showmode();
	printstack(1,stack);
	return GOODOP;
}

opreturn
modedec(void)
{
	mode = 'D';
	showmode();
	printstack(1,stack);
	return GOODOP;
}

opreturn
modefloat(void)
{
	mode = 'F';
	showmode();
	printstack(1,stack);
	return GOODOP;
}

opreturn
separators(void)
{
	if (!thousands_sep[0]) {
		p_printf(" No thousands separator found in the "
			"current locale. so numeric separators are disabled\n");
		digitseparators = 0;
		return GOODOP;
	}

	if (!toggler(&digitseparators, "Numeric separators now", "on", "off"))
		return BADOP;

	return GOODOP;
}

void
float_mode_messages(int both)
{
	if (both)
		p_printf(" Will show floating point in %s format\n", float_specifier);

	if (mode != 'F')
		p_printf(" Not in floating mode, preference"
				" recorded but ignored.\n");
}

opreturn
automatic(void)
{
	float_specifier = "automatic";
	float_mode_messages(1);

	return GOODOP;
}

opreturn
engineering(void)
{
	float_specifier = "engineering";
	float_mode_messages(1);

	return GOODOP;
}

opreturn
fixedpoint(void)
{
	float_specifier = "fixed decimal";
	float_mode_messages(1);

	return GOODOP;
}

opreturn
digits(void)
{
	ldouble digits;
	char *limited = "";

	if (!pop(&digits))
		return BADOP;

	float_digits = abs((int)digits);

	// but it can't be greater than our maximum precision
	if (float_digits > max_precision) {
		float_digits = max_precision;
		limited = "the maximum of ";
	} else if (float_digits < 0) {
		float_digits = 0;
	}

	p_printf(" Floating formats configured for %s%d digit%s.\n", limited,
		float_digits, float_digits == 1 ? "" : "s");

	float_mode_messages(0);

	return GOODOP;
}

void
setup_width(int bits)
{
	/* we use long double to store our data.  in integer mode,
	 * this means the FP mantissa, if it's shorter than long long,
	 * may limit our maximum word width.  */
	if (!bits || !max_int_width) {	/* first call */
		max_int_width = LONGLONG_BITS;
		if (max_int_width > LDBL_MANT_DIG)
			max_int_width = LDBL_MANT_DIG;
		bits = max_int_width;
	}

	if (bits > max_int_width)
		bits = max_int_width;

	int_width = bits;
	int_sign_bit = (1LL << (int_width - 1));

	if (int_width == LONGLONG_BITS) {
		int_mask = ~0;
		int_max = LLONG_MAX;
		int_min = LLONG_MIN;
	} else {
		int_mask = (1LL << int_width) - 1;
		int_max = int_mask >> 1;
		int_min = int_sign_bit;
	}
}

void
mask_stack(void)
{
	struct num *s;
	for (s = stack; s; s = s->next) {
		if (isfinite(s->val))
			s->val = sign_extend((long long)s->val & int_mask);
	}
}

opreturn
width(void)
{
	ldouble n;
	long long bits;

	if (!pop(&n))
		return BADOP;

	bits = n;
	if (bits == 0) {
		bits = max_int_width;
	} else if (bits > max_int_width) {
		bits = max_int_width;
		p_printf(" Width out of range, set to max (%lld)\n", bits);
	} else if (bits < 2) {
		bits = 2;
		p_printf(" Width out of range, set to min (%lld)\n", bits);
	}

	setup_width(bits);

	p_printf(" Integers are now %d bits wide.\n", int_width);
	if (floating_mode(mode)) {
		p_printf(" In floating mode, integer width"
				" is recorded but ignored.\n");
	} else {
		mask_stack();
	}

	return GOODOP;
}

opreturn
zerof(void)
{
	return toggler(&zerofill, "Zero fill in hex/octal/binary modes is now",
		"on", "off");
}

opreturn
rightalign(void)
{
	return toggler(&rightalignment, "Right alignment of integer modes is now",
		"on", "off");
}

/* for store/recall */
ldouble offstack;

opreturn
store(void)
{
	ldouble a;

	if (peek(&a)) {
		offstack = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
recall(void)
{
	push(offstack);
	return GOODOP;
}

opreturn
push_pi(void)
{
	result_push(pi);
	return GOODOP;
}

opreturn
push_e(void)
{
	result_push(e);
	return GOODOP;
}

opreturn
mark(void)
{
	ldouble n;
	if (!pop(&n))
		return BADOP;

	if (n > stack_count || n < -1) {
		if (stack_count == 0)
			error(" error: bad mark, max of 0 with empty stack, or, -1 to clear\n");
		else
			error(" error: bad mark, range between 0 and stack length (%d), or -1 to clear\n", stack_count);
	}

	if (n == -1)
		stack_mark = 0; // special case:  clear the mark
	else
		stack_mark = stack_count - n;
	return GOODOP;
}

struct num *snapstack;

opreturn
snapshot(void)
{
	struct num *p;

	if (stack_count <= stack_mark) {
		error(" error: nothing to snapshot\n");
		return BADOP;
	}

	p = stack;
	if (!p) { // if stack count/mark are correct, this can't happen
		error(" empty stack\n");
		return BADOP;
	}

	// clear existing snapstack
	while ((p = snapstack)) {
		snapstack = p->next;
		free(p);
	}

	// copy (as much as we want of the) real stack to snapstack
	p = stack;
	snapstack = NULL;
	int n = stack_count;
	while (n > stack_mark) {
		struct num *np;

		// push p->val on snapstack
		np = (struct num *)calloc(1, sizeof(struct num));
		if (!np)
			memory_failure();
		np->val = p->val;
		np->next = snapstack;
		snapstack = np;

		// next item from "real" stack
		p = p->next;
		n--;
	}

	return GOODOP;
}

opreturn
restore(void)
{
	struct num *p = snapstack;
	while (p) {
		push(p->val);
		p = p->next;
	}
	return GOODOP;
}

opreturn
sum_worker(boolean do_sum)
{
	opreturn r;
	ldouble a, tot = 0, n = 0;

	if (stack_count <= stack_mark) {
		error(" error: nothing to %s\n", do_sum ? "sum":"avg");
		return BADOP;
	}

	while (stack_count > stack_mark) {
		if ((r = pop(&a)) == BADOP)
			break;
		tot += a;
		n++;
	}

	stack_mark = 0;

	if (floating_mode(mode))
		result_push(do_sum ? tot : tot/n );
	else
		push(do_sum ? tot : (tot/n) );

	return r;
}

opreturn
sum(void)
{
	return sum_worker(1);
}

opreturn
avg(void)
{
	return sum_worker(0);
}

opreturn
units_in_mm(void)
{
	ldouble a;

	if (pop(&a)) {
		a *= 25.4;
		result_push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_mm_in(void)
{
	ldouble a;

	if (pop(&a)) {
		a /= 25.4;
		result_push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_ft_m(void)
{
	ldouble a;

	if (pop(&a)) {
		a /= 3.28084;
		result_push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_m_ft(void)
{
	ldouble a;

	if (pop(&a)) {
		a *= 3.28084;
		result_push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_F_C(void)
{
	ldouble a;

	if (pop(&a)) {
		a -= 32.0;
		a /= 1.8;
		result_push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_C_F(void)
{
	ldouble a;

	if (pop(&a)) {
		a *= 1.8;
		a += 32.0;
		result_push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_l_qt(void)
{
	ldouble a;

	if (pop(&a)) {
		a *= 1.05669;
		result_push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_qt_l(void)
{
	ldouble a;

	if (pop(&a)) {
		a /= 1.05669;
		result_push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_oz_g(void)
{
	ldouble a;

	if (pop(&a)) {
		a *= 28.3495;
		result_push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_g_oz(void)
{
	ldouble a;

	if (pop(&a)) {
		a /= 28.3495;
		result_push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_oz_ml(void)
{
	ldouble a;

	if (pop(&a)) {
		a *= 29.5735;
		result_push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_ml_oz(void)
{
	ldouble a;

	if (pop(&a)) {
		a /= 29.5735;
		result_push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_mi_km(void)
{
	ldouble a;

	if (pop(&a)) {
		a /= 0.6213712;
		result_push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_km_mi(void)
{
	ldouble a;

	if (pop(&a)) {
		a *= 0.6213712;
		result_push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_deg_rad(void)
{
	ldouble a;

	if (pop(&a)) {
		a = to_radians(a);
		result_push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
units_rad_deg(void)
{
	ldouble a;

	if (pop(&a)) {
		a = to_degrees(a);
		result_push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}


opreturn
units_mpg_l100km(void)
{
	/* the same formula converts back and
	 * forth between mpg and liters/100km */
	ldouble a;

	if (pop(&a)) {
		a = 235.214583 / a;
		result_push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

token *out_stack, *oper_stack, *infix_rpn_queue;

char *
stackname(token **tstackp)
{
	char *n;
	if (tstackp == &oper_stack)
		n = "operator stack";
	else if (tstackp == &out_stack)
		n = "output stack";
	else if (tstackp == &infix_rpn_queue)
		n = "rpn output";
	else
		n = "unknown stack";
	return n;
}


void
tpush(token **tstackp, token *tok)
{
	struct token *t;

	/* if we originally malloc'ed the incoming token, just reuse
	 * it, otherwise malloc and copy.  */
	if (tok->alloced) {
		t = tok;
	} else {
		t = (struct token *)calloc(1, sizeof(struct token));
		if (!t)
			memory_failure();

		*t = *tok;
		t->alloced = 1;
	}

	t->next = *tstackp;
	*tstackp = t;
}

token *
tpeek(token **tstackp)
{
	return *tstackp;
}

token *
tpop(token **tstackp)
{
	struct token *rt;

	rt = *tstackp;
	if (!rt) {
		return NULL;
	}

	*tstackp = (*tstackp)->next;

	return rt;
}

void
tclear(token **tstackp)
{
	token *t, *nt;

	t = *tstackp;
	*tstackp = NULL;

	while (t) {
		nt = t->next;
		if (t->type == VARIABLE && t->val.varname)
			free(t->val.varname);
		free(t);
		t = nt;
	}
}

void
sprint_token(char *s, int slen, token *t)
{
	switch (t->type) {
	case NUMERIC:
		snprintf(s, slen, "'%.*Lg'", max_precision, t->val.val);
		break;
	case SYMBOLIC:
		snprintf(s, slen, "'%s'", t->val.oper->name);
		break;
	case VARIABLE:
		snprintf(s, slen, "'%s'", t->val.varname);
		break;
	case OP:
		snprintf(s, slen, "'%s'", t->val.oper->name);
		break;
	case EOL:
		snprintf(s, slen, "'EOL'");
		break;
	case UNKNOWN:
		snprintf(s, slen, "'unknown (%d)'", t->type);
		break;
	default:
		error(" BUG: hit default in sprint_token()\n");
	}
}

void
show_tok(token *t)
{
	static char buf[128];

	sprint_token(buf, sizeof(buf), t);
	printf(" %s ", buf);
}

void
stack_dump(token **tstackp)
{
	token *t = *tstackp;

	printf("%s: ", stackname(tstackp));
	if (!t)
		printf("<empty>");
	else
		while (t) {
			show_tok(t);
			t = t->next;
		}
	printf("\n");
}

void
tdump(token **tstackp)
{
	if (tracing > 1)
		stack_dump(tstackp);
}

token open_paren_token, chsign_token, nop_token;

void
create_infix_support_tokens()
{
	/* we need a couple of token identifiers for later on,
	 * specifically for dealing with infix processing.  */
	char *outp;
	(void)parse_tok("(", &open_paren_token, &outp, 0);
	(void)parse_tok("chs", &chsign_token, &outp, 0);
	(void)parse_tok("nop", &nop_token, &outp, 0);
}

void
expression_error(token *pt, token *t)
{
	char pts[128], ts[128];
	sprint_token(pts, 128, pt);
	sprint_token(ts, 128, t);
	error(" error: bad expression sequence, at %s and %s\n", pts, ts);
}

opreturn
close_paren(void)
{
	/* this has to be a warning -- the command in error is already
	 * finished, so we can't cancel it. */
	error(" warning: mismatched/extra parentheses\n");
	return BADOP;
}

opreturn
semicolon(void)
{
	/* for infix:  similar to the comma operator in C:
	 *     (y, x) discards y, returns x
	 * so in RPN (perhaps less useful):
	 *      y x ;  discards y (i.e., 2nd from top of stack), keeps x
	 */
	ldouble a, b;
	if (pop(&b)) {
		if (pop(&a)) {
			push(b);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

boolean
prev_tok_was_operand(token *pt)
{
	return pt->type == NUMERIC ||
		pt->type == SYMBOLIC ||
		pt->type == VARIABLE ||
		(pt->type == OP && pt->val.oper->func == close_paren);
}

#define t_op t->val.oper	// shorthands.  don't use unless type == OP
#define tp_op tp->val.oper
#define pt_op pt->val.oper

void shunt(token *t)
{
	token *tp;

	while ((tp = tpeek(&oper_stack))) {

		if (tp->type == OP) {
			if (strcmp(tp_op->name, "(") == 0)
				break;
			if (tp_op->prec < t_op->prec)
				break;
			if (tp_op->prec == t_op->prec && t_op->assoc == 'R')
				break;
		}
		tpush(&out_stack, tpop(&oper_stack));
	}
	tpush(&oper_stack, t);
}

/* This implementation of Dijkstra's shunting yard algorithm is based
 * on pseudocode from Wikipedia and brilliant.org, on several of the
 * coded examples at rosettacode.org, and on pseudo-code generated by
 * an AI bot.  */
opreturn
open_paren(void)
{
	static token tok, prevtok;
	int paren_count;
	token *t, *pt, *tp;	// pointers to tok, prevtok, and tpeek'ed token

	tclear(&out_stack);
	tclear(&oper_stack);

	if (tracing)
		printf("infix tokens: ");

	// push the '(' token the user typed
	tpush(&oper_stack, &open_paren_token);
	paren_count = 1;
	prevtok = open_paren_token;
	pt = &prevtok;

	while (1) {
		trace(("\n"));
		tdump(&oper_stack);
		tdump(&out_stack);

		while (isspace(*input_ptr))
			input_ptr++;

		if (!*input_ptr)
			break;

		t = &tok;

		if (!parse_tok(input_ptr, &tok, &input_ptr, 0)) {
			goto cleanup;
		}

		/* we delayed classifying the previous variable token
		 * until we could tell if we were assigning or not.
		 * we do it here. */
		if (pt->type == VARIABLE) {
			/* i.e., is it "r1 = 3" or "r1 + 3"? */
			if (t->type == OP && t_op->func == assignment)
				tpush(&oper_stack, pt);
			else
				tpush(&out_stack, pt);
		}

		switch (t->type) {
		case VARIABLE:
			if (prev_tok_was_operand(pt)) {
				expression_error(pt, t);
				input_ptr = NULL;
				return BADOP;
			}
			/* do nothing now.  we need to know what comes
			 * next:  "r1 = 3" is very different than "r1 + 3" */
			break;
		case NUMERIC:
		case SYMBOLIC:
			if (prev_tok_was_operand(pt)) {
				expression_error(pt, t);
				input_ptr = NULL;
				return BADOP;
			}
			tpush(&out_stack, t);
			break;
		case OP:
			if (t_op->func == open_paren) {
				if (prev_tok_was_operand(pt)) {
					expression_error(pt, t);
					input_ptr = NULL;
					return BADOP;
				}
				// Push opening parenthesis to operator stack
				tpush(&oper_stack, t);
				paren_count++;
			} else if (t_op->func == close_paren) {
				if (!prev_tok_was_operand(pt)) {
					expression_error(pt, t);
					input_ptr = NULL;
					return BADOP;
				}
				// Process until matching opening paren
				while ((tp = tpeek(&oper_stack))) {
					if (tp->type == OP &&
						tp_op->func == open_paren)
						break;

					tpush(&out_stack, tpop(&oper_stack));
				}

				// Pop the opening parenthesis
				free(tpop(&oper_stack));
				tp = tpeek(&oper_stack);
				if (tp && tp_op->operands == 1) {
					tpush(&out_stack, tpop(&oper_stack));
				}
				paren_count--;

			} else if (t_op->operands == 1) { // just one operand
			unary:
				if (prev_tok_was_operand(pt)) {
					expression_error(pt, t);
					input_ptr = NULL;
					return BADOP;
				}

				shunt(t);

			} else if (t_op->operands == 2) { // two operands
				/*
				 * +/- are unary if the previous token isn't
				 *   something that will become an operand,
				 *   and if they're followed by something that
				 *   isn't space or the end of expression.
				 *   Also, we don't let them be followed by
				 *   +/-, to prevent weird multiple +-+
				 *   sequences.  If that's not all true,
				 *   they're binary operators.
				 * TL;DR: +/- are unary if prev token is a
				 *   non-')' operator and next char is
				 *   whitespace, ), +, -, or \0
				 */
				if ( (t_op->func == subtract ||
				      t_op->func == add) &&
					!prev_tok_was_operand(pt) &&
					!strchr(" \t\v\r\n)+-", *input_ptr)) {
					if (t_op->func == subtract) {
						t = &chsign_token;
						trace((" subtract is now chs\n"));
					} else {  // add
						t = &nop_token;
						trace((" add is now nop\n"));
					}
					goto unary;
				}

				if (t_op->func == assignment) {
					if (pt->type != VARIABLE) {
						expression_error(pt, t);
						input_ptr = NULL;
						return BADOP;
					}
					tpush(&oper_stack, t);
					break;
				}

				if (!prev_tok_was_operand(pt)) {
					expression_error(pt, t);
					input_ptr = NULL;
					return BADOP;
				}

				shunt(t);

			} else {
				error(" error: '%s' unsuitable in infix expression\n",
					t_op->name);
				input_ptr = NULL;
				return BADOP;
			}
			break;

		default:
		case UNKNOWN:
		cleanup:
			input_ptr = NULL;
			return BADOP;
		}

		if (paren_count == 0)
			break;

		prevtok = *t;

	}
	trace(("\nfinished reading expression\n"));

	if (paren_count) {
		error(" error: missing parentheses\n");
		return BADOP;
	}

	/* the shunting yard is finished.  output stack is in the
	 * wrong order, so one more transfer to reverse it.
	 * gettoken() will pull from this copy.
	 */
	while((t = tpop(&out_stack)) != NULL) {
		tpush(&infix_rpn_queue, t);
	}

	if (tracing) {
		trace(("\nmerged and reversed:\n"));
		printf("\n");
		stack_dump(&infix_rpn_queue);
	}

	return GOODOP;

#undef t_op
#undef tp_op
}


opreturn
autop(void)
{
	return toggler(&autoprint, "Autoprinting is now", "on", "off");
}

/* debug support */
opreturn
tracetoggle(void)
{
	ldouble wanttracing;

	if (!pop(&wanttracing))
		return BADOP;

	if (wanttracing == 11) {
		opreturn commands(void);
		return commands();
	}

	// two levels currently, mostly for infix processing.
	// 1 is just logs input and rpn tokens
	// 2 is full shunting algorithm logging, plus also snapping/rounding
	tracing = wanttracing;

	p_printf(" internal tracing is now level %d\n", tracing);
	return GOODOP;
}

opreturn
rounding(void)
{
	return toggler(&do_rounding, "Float snapping/rounding is now",
		"on", "off");
}

void
exitret(void)
{
	ldouble a = 0;
	if (!stack)
		exit(2);  // exit 2 on empty stack

	pop(&a);

	exit(a == 0);  // flip exit status, per unix convention

}

opreturn
quit(void)
{
	if (autoprint) {
		print_top(mode);
		pending_show();
	}

	exitret();
	return GOODOP; // not reached
}

typedef struct {
    ldouble value;
    char *name;
} dynvar;

#define NVAR 50
dynvar variables[NVAR];

int
comparevars(const void *a, const void *b)
{
	dynvar *va = (dynvar *)a;
	dynvar *vb = (dynvar *)b;

	return strcmp(va->name, vb->name);
}

opreturn
showvars(void)
{
	dynvar *v;

	if (!variables->name) {
		p_printf(" <none>\n");
		return GOODOP;
	}
	for (v = variables; v->name; v++)
		/* count the variables */;

	qsort(variables, v - variables, sizeof(*v), comparevars);

	int savealign = rightalignment;
	rightalignment = 0;
	for (v = variables; v->name; v++) {
		p_printf(" %20s ", v->name);
		print_n(&v->value, mode, 0);
	}
	rightalignment = savealign;

	return GOODOP;
}

dynvar *
findvar(char *name)
{
	dynvar *v;
	for (v = variables; v->name && v < variables + NVAR-1; v++) {
		if (strcmp(name, v->name) == 0) {
			return v;
		}
	}
	if (v < variables + NVAR-1) {
		v->name = strdup(name);
		return v;
	}
	return 0;
}

int
dynamic_var(token *t)
{
	dynvar *v;

	v = findvar(t->val.varname);
	free(t->val.varname);
	t->val.varname = 0;
	if (!v) {
		error(" error: out of space for variables\n");
		return 0;
	}

	/* if we were preceded by '=', set our value */
	if (variable_write_enable) {
		ldouble a;
		if (!peek(&a))
			return 0;
		v->value = a;
	} else {
		push(v->value);
	}
	return 1;
}

size_t stralnum(char *s, char **endptr)
{
	char *ns = s;
	while (isalnum(*ns) || *ns == '_')
		ns++;
	*endptr = ns;
	return ns - s;
}

int
parse_tok(char *p, token *t, char **nextp, boolean parsing_rpn)
{
	int sign = 1;
	int n;

	if (parsing_rpn && (*p == '+' || *p == '-')) {
		/* In RPN, be sure + and - are bound closely to
		 * numbers.  We want "1 2 -3" to push "1", "2", and
		 * "-3", not "(1-2)", and "3".  For infix expressions,
		 * whether to bind the +/- to a number depends on what
		 * came before.  "(2-3)" should be read as "(2 - 3)",
		 * not "(2 -3)".  That's handled in open_paren(),
		 * where we can track ordering.
		 */
		if (*p == '+' && (match_dp(p + 1) || isdigit(*(p + 1)))) {
			p++;
		} else if (*p == '-' && (match_dp(p + 1) || isdigit(*(p + 1)))) {
			sign = -1;
			p++;
		} else if (isspace(*(p+1)) || *(p+1) == 0) {
			goto is_oper;
		} else {
			goto unknown;
		}
	}

	if (*p == '0' && (*(p + 1) == 'x' || *(p + 1) == 'X')) {
		// hex, leading "0x"
		long double dd;
		p += 2;
		if (raw_hex_input_ok) {
			// will accept floating hex like 0xc.90fdaa22168c23cp-2
			dd = strtold(p, nextp);
		} else {
			// accept simple hex integers only
			dd = strtoull(p, nextp, 16);
		}

		/* be strict about what comes next */
		if (isalnum(**nextp))
			goto unknown;

		t->val.val = dd * sign;
		t->type = NUMERIC;
		t->imode = 'H';

	} else if (*p == '0' && (*(p + 1) == 'b' || *(p + 1) == 'B')) {
		// binary, leading "0b"
		p += 2;
		long long ln = strtoull(p, nextp, 2);

		/* be strict about what comes next */
		if (*nextp == p || isalnum(**nextp))
			goto unknown;

		t->type = NUMERIC;
		t->imode = 'B';
		t->val.val = ln * sign;

	} else if (*p == '0' && (*(p + 1) == 'o' || *(p + 1) == 'O')) {
		// octal, leading "0o"
		p += 2;
		long long ln = strtoull(p, nextp, 8);

		/* be strict about what comes next */
		if (*nextp == p || isalnum(**nextp))
			goto unknown;

		t->type = NUMERIC;
		t->imode = 'O';
		t->val.val = ln * sign;

	} else if (isdigit(*p) || match_dp(p)) {
		// decimal
		long double dd = strtold(p, nextp);

		/* don't be strict about what comes next.  mistakes are
		 * less likely when entering decimal. this makes 3k or 18w
		 * legal */
		if (p == *nextp)
			goto unknown;

		t->type = NUMERIC;
		t->imode = 'D';
		t->val.val = dd * sign;
	} else if (*p == '_' && isalnum(*(p+1))) {
		// variable
		n = stralnum(p, nextp);
		t->type = VARIABLE;
		t->val.varname = strndup(p,n);
	} else {

	    is_oper:
		if (isalpha(*p)) {
			n = stralnum(p, nextp);
		} else if (ispunct(*p)) {
			/* parser hack:  hard-coded list of
			 * all double-punctuation operators */
			if (    (p[0] == '>' && p[1] == '>') ||      //   >>
				(p[0] == '<' && p[1] == '<') ||      //   <<
				(p[0] == '>' && p[1] == '=') ||      //   >=
				(p[0] == '<' && p[1] == '=') ||      //   <=
				(p[0] == '=' && p[1] == '=') ||      //   ==
				(p[0] == '!' && p[1] == '=') ||      //   !=
				(p[0] == '&' && p[1] == '&') ||      //   &&
				(p[0] == '|' && p[1] == '|') ||      //   ||
				(p[0] == '*' && p[1] == '*')) {      //   **
				n = 2;
			} else {
				n = 1;
			}
			*nextp = p + n;
		} else {
			error(" error: illegal character in input\n");
			t->val.str = p;
			t->type = UNKNOWN;
			return 0;
		}

		// command
		oper *op;

		op = opers;
		while (op->name) {
			int matchlen;

			if (!op->func) {
				op++;
				continue;
			}
			matchlen = strlen(op->name);
			if (n == matchlen && !strncmp(op->name, p, matchlen)) {
				*nextp = p + matchlen;
				t->val.oper = op;
				if (op->operands == Sym) // like "pi", "recall"
					t->type = SYMBOLIC;
				else
					t->type = OP;
				break;
			}
			op++;
		}
		if (!op->name) {
		unknown:
			error(" error: unrecognized input '%s'\n",
				strtok(p, " \t\n"));
			t->val.str = p;
			t->type = UNKNOWN;
			return 0;
		}
	}
	if (tracing) show_tok(t);
	return 1;
}

char *
strremoveall(char *haystack, const char *needle)
{
	size_t len = strlen(needle);

	if (len > 0) {
		char *p = haystack;

		while ((p = strstr(p, needle)) != NULL)
			memmove(p, p + len, strlen(p + len) + 1);
	}
	return haystack;
}

void
no_comments(char *cp)
{
	char *ncp;

	/* Eliminate comments */
	if ((ncp = strchr(cp, '#')) != NULL)
		*ncp = '\0';

	/* Eliminate the thousands separator from numbers, like
	 * "1,345,011".  This removes them from the entire line, which
	 * would be a problem except:  the only simple ascii
	 * separators ever used in locales are '.' and ','.  We don't
	 * ',' use anywhere else.  Removing '.' is safe, because if
	 * the separator is '.', then the decimal point isn't.  All
	 * the other separators are unicode sequences, which we also
	 * don't use.  So the command line won't be harmed by this
	 * removal.  Some locales use a space as a separator, but it's
	 * a "hard" space, represented as unicode.  */
	if (thousands_sep_input[0])
		strremoveall(cp, thousands_sep_input);

	/* Same for currency symbols.  They're mostly unicode
	 * sequences or punctuation (e.g., '$'), which are safe to
	 * remove.  But some are plain ascii.  We checked earlier to
	 * be sure the currency symbol doesn't match any of our
	 * commands.  */
	if (currency && currency[0])
		strremoveall(cp, currency);
}

#ifdef USE_READLINE
/* This supports readline command completion */
char *
command_generator(const char *prefix, int state)
{
	static int len;
	static struct oper *op;

	/* If this is the first time called, initialize our state. */
	if (!state) {
		op = opers - 1;
		len = strlen(prefix);
	}

	/* Return the next name in the list that matches our prefix. */
	while (op++) {
		if (!op->name)
			break;
		if (!op->name[0])
			continue;
		if (!op->func)
			continue;
		if (strncmp(op->name, prefix, len) != 0)
			continue;
		return strdup(op->name);
	}

	return 0;
}

/* Attempted completion function. */
char **
command_completion(const char* prefix, int start, int end)
{
	(void)start;  // suppress "unused" warnings
	(void)end;

	return rl_completion_matches(prefix, command_generator);
}
#endif

static int o_stdout_fd = -1;

void
suppress_stdout(void)
{
	fflush(stdout);

	o_stdout_fd = dup(STDOUT_FILENO);

	int null_fd = open("/dev/null", O_WRONLY);
	if (null_fd == -1) {
		o_stdout_fd = -1;
		return;
	}

	dup2(null_fd, STDOUT_FILENO);	// Redirect stdout to /dev/null
	close(null_fd);		// Close the /dev/null file descriptor
}

void
restore_stdout(void)
{
	if (o_stdout_fd == -1)
		return;
	fflush(stdout);
	dup2(o_stdout_fd, STDOUT_FILENO);	// Restore original stdout
	close(o_stdout_fd);
	o_stdout_fd = -1;
}

/* on return from fetch_line(), the global input_ptr is a string
 * containing commands to be executed */
int
fetch_line(void)
{
	static int arg = 1;
	static char *input_buf;
	static size_t blen;
	static boolean tried_rca_init;
	char *rca_init;

	if (!tried_rca_init) {
		tried_rca_init = TRUE;
		rca_init = getenv("RCA_INIT");
		if (rca_init) {
			suppress_stdout();
			blen = strlen(rca_init) + 1;
			input_buf = malloc(blen);
			if (!input_buf)
				memory_failure();
			strcpy(input_buf, rca_init);
			input_ptr = input_buf;
			return 1;
		}
	}

	restore_stdout();

	/* if there are args on the command line, they're taken as
	 * initial commands.  since only numbers can start with '-',
	 * we let any other use of a hyphen bring up a usage message.
	 */
	if (arg < g_argc) {

		if (g_argv[1][0] == '-' && !(isdigit(g_argv[1][1])))
			usage();

		if (input_buf) free(input_buf);

		blen = 0;
		for (arg = 1; arg < g_argc; arg++)
			blen += strlen(g_argv[arg]) + 2;

		input_buf = malloc(blen);
		if (!input_buf)
			memory_failure();

		*input_buf = '\0';
		for (arg = 1; arg < g_argc; arg++) {
			strcat(input_buf, g_argv[arg]);
			strcat(input_buf, " ");
		}

		no_comments(input_buf);

		input_ptr = input_buf;
		return 1;
	}

#ifdef USE_READLINE
	static char readline_init_done = 0;

	if (!readline_init_done) {
		rl_readline_name = "rca";
		rl_basic_word_break_characters = " \t\n";
		rl_attempted_completion_function = command_completion;
		using_history();
		readline_init_done = 1;
	}

	/* if we used the buffer as input, add it to history.  doing
	 * this here records any command line input, possibly stored
	 * in the buffer above, on the first call to fetch_line() */
	if (input_buf && *input_buf)
		add_history(input_buf);

	if (input_buf) free(input_buf);

	if ((input_buf = readline("")) == NULL)  // got EOF
		exitret();

#if READLINE_NO_ECHO_BARE_NL
	/* a bug in readline() doesn't echo bare newlines to a tty if
	 * the program has no prompt.  so we do it here.  this is
	 * needed in some sub-versions of readline 8.2 */
	if (*input_buf == '\0')
		putchar('\n');
#endif

#else // no readline()

	if (getline(&input_buf, &blen, stdin) < 0)  // EOF
		exitret();

	if (input_buf[strlen(input_buf) - 1] == '\n')
		input_buf[strlen(input_buf) - 1] = '\0';

	/* if stdin is a terminal, the command is already on-screen.
	 * but we also want it mixed with the output if we're
	 * redirecting from a file or pipe.  easy to get rid of it
	 * with something like:   rca < commands | grep '^ '
	 */
	if (!isatty(0))
		printf("%s\n", input_buf);
#endif

	no_comments(input_buf);

	input_ptr = input_buf;

	return 1;
}

int
gettoken(struct token *t)
{
	char *next_input_ptr;
	if (input_ptr == NULL) {
		if (!fetch_line())
			return 0;
	}

	while (isspace(*input_ptr))
		input_ptr++;

	if (*input_ptr == '\0') {	/* out of input */
		t->type = EOL;
		if (tracing) show_tok(t);
		input_ptr = NULL;
		return 1;
	}

	fflush(stdin);

	if (!parse_tok(input_ptr, t, &next_input_ptr, 1)) {
		input_ptr = NULL;
		return 0;
	}

	input_ptr = next_input_ptr;
	return 1;
}

/* useful for resetting width from debugger, to generate the
 * (narrower) man page copy of the precedence table. */
int precedence_width = 70;

opreturn
precedence(void)
{
	oper *op;
#define NUM_PRECEDENCE 34
	static int pass = 0;
	static char assoc[NUM_PRECEDENCE];
	static char *prec_ops[NUM_PRECEDENCE];
	int linelen[NUM_PRECEDENCE] = {0};
	char *prefix[NUM_PRECEDENCE] = {0};
	int prec, i;

	p_printf(" Precedence for operators in infix expressions, from \n"
	       "  top to bottom in order of descending precedence.\n"
	       " All operators are left-associative, except for those\n"
	       "  in rows marked 'R', which associate right to left.\n");

	/* do single char commands first, then the rest, then never
	 * regenerate the table again.  */
	for ( ; pass < 2; pass++) {

		op = opers;
		while (op->name) {

			/* skip anything in the table that doesn't have
			 * a name, a function, or a precedence */
			if (!op->name[0] || !op->func || op->prec == 0) {
				op++;
				continue;
			}

			/* only do one character names in the first pass,
			 * and multi-char names in the second */
			if ((pass == 0) ^ (op->name[1] == 0)) {
				op++;
				continue;
			}

			if (op->prec >= NUM_PRECEDENCE) {
				error("error: %s precedence too large: %d\n",
					op->name, op->prec);
			}

			if (!prec_ops[op->prec]) {
				prec_ops[op->prec] = (char *)calloc(1, 500);
				if (!prec_ops[op->prec])
					memory_failure();
				prefix[op->prec] = "";
				linelen[op->prec] = 12;
			}
			if (strcmp(op->name, "chs") == 0) {
				strcat(prec_ops[op->prec], "+ - ");
				linelen[op->prec] += 4;
			}
			if (!assoc[op->prec]) {
				assoc[op->prec] = op->assoc;
			} else {
				if (assoc[op->prec] != op->assoc)
					error(" error: associativity bug, op %s\n", op->name);
			}
			strcat(prec_ops[op->prec], op->name);
			strcat(prec_ops[op->prec], " ");
			linelen[op->prec] += strlen(op->name) + 1;
			if (linelen[op->prec] > precedence_width) {
				linelen[op->prec] = 12;
				prefix[op->prec] = "\n               ";
			} else {
				strcat(prec_ops[op->prec], prefix[op->prec]);
				prefix[op->prec] = "";
			}
			op++;
		}
	}

	/* Our internal precedence numbers aren't necessarily
	 * something to show the user.  The rows are renumbered
	 * "nicely" as we emit rows we gathered above. */
	i = 1;
	for (prec = NUM_PRECEDENCE-1; prec >=0; prec--) {
		if (prec_ops[prec]) {
			p_printf(" %-2i  %c     %s\n", i,
			assoc[prec] ? 'R' : ' ',
			prec_ops[prec]);
			i++;
		}
	}

	return GOODOP;
}

/* This is a mostly raw dump of the oper[] table, since the structure
 * layout in the source is a little hard to read.  */
opreturn
commands(void)
{
	oper *op, *lastop = NULL;

	op = opers;

	p_printf("%s %s %s %s %s %s\n",
	       "oper", "alias", "oprnds", "prc", "ass", "help");
	p_printf("---- ----- ------ --- --- ---------\n");
	while (op->name) {
		if (op->func ) {
			int name_fmt;
			if (lastop && lastop->func == op->func )
				name_fmt = 10;
			else
				name_fmt = -10;

			p_printf("%*s  %2d    %2d  %c   %s\n", name_fmt,
				op->name, op->operands, op->prec,
				op->assoc ? 'R' : ' ',
				op->help ? op->help : "");
		}
		lastop = op;
		op++;
	}
	return GOODOP;
}

opreturn
license(void)
{
	int i = 0;

	while (licensetext[i])
		p_printf("%s\n", licensetext[i++]);

	return GOODOP;
}

char *
getversion(void)
{
	static char vbuf[100];
	if (!ccprogversion[0] || strcmp(ccprogversion, progversion) == 0) {
		snprintf(vbuf, sizeof(vbuf), "version %s",
			progversion);
	} else {
		snprintf(vbuf, sizeof(vbuf), "version %s (%s)",
			progversion, ccprogversion);
	}
	return vbuf;
}

opreturn
version(void)
{
	p_printf(" %s\n", getversion());
	return GOODOP;
}

opreturn
help(void)
{
	oper *op;

	FILE *fout;
	boolean fout_is_pipe = 0;
	char *pager = getenv("PAGER");

	if (pager && pager[0] && isatty(fileno(stdout)) &&
			(fout = popen(pager, "w"))) {
		p_printf("Using '%s' (from $PAGER) to show help text\n", pager);
		fout_is_pipe = 1;
	} else {
		fout = stdout;
	}



	op = opers;
	fprintf(fout, "\
 rca -- a rich/RPN scientific and programmer's calculator\n\
  Any arguments on the command line are used as initial calculator input.\n\
  Entering a number pushes it on the stack.\n\
  Operators replace either one or two stack values with their result.\n\
  Most whitespace is optional between numbers and operators.\n\
  Input can include locale currency%s symbols: %s12%s345%s67\n\
  Always prefix hex (0x7f) or octal (0o177) input, even in hex or octal mode.\n\
  Infix expressions are entered using (...), as in: (sin(30)^2 + cos(30)^2)\n\
  Below, 'x' refers to top-of-stack, 'y' refers to the next value beneath.\n\
  rca's normal exit value reflects the logical value of the top of stack.\n\
\n\
",
	thousands_sep_input[0] ? " and grouping" : "",
		currency, thousands_sep_input, decimal_pt);

	char cbuf[1000];
	opfunc prevfunc;

	cbuf[0] = '\0';
	prevfunc = 0;

	while (op->name) {
		if (!*op->name) {
			fprintf(fout, "\n");
		} else {
			if (!op->func) {
				fprintf(fout, " %s\n", op->name);
			} else {
				if (cbuf[0]) { // continuing
					if (op->func == prevfunc)
						strcat(cbuf, op->help ?
							", or " : ", ");
					else
						strcat(cbuf, ", ");
				} else {
					strcat(cbuf, " ");
				}
				strcat(cbuf, op->name);
				if (op->help) {
					fprintf(fout, "%21s     %s\n",
						cbuf, op->help);
					cbuf[0] = '\0';
				}
			}
		}
		prevfunc = op->func;
		op++;
	}
	fprintf(fout, "\n%78s\n",  getversion());

	if (!fout_is_pipe) {
		// tip not needed if a pager's already in use
		fprintf(fout, "\n Tip:	Use \"rca help q | less\""
				" to view this help\n");
		return GOODOP;
	}

	if (pclose(fout) != 0)
		p_printf(" Failed showing help. Unset PAGER to show help directly\n");
	else
		p_printf(" (Help ended)\n");

	return GOODOP;
}

void
locale_init(void)
{
	struct lconv *lc;

	setlocale(LC_ALL, "");

	lc = localeconv();

	/* fetch the decimal point */
	decimal_pt = lc->decimal_point;
	decimal_pt_len = strlen(decimal_pt);

	/* fetch the thousands separator
	 * thousands_sep will be used only for output
	 * thousands_sep_input only for input */
	thousands_sep_input = thousands_sep = lc->thousands_sep;

	/* if there's no thousands separator, default the input
	 * version (which will be used to clean up program input) to
	 * ",", but only if the decimal point is ".".  this lets us
	 * safely strip commas from input even if the locale isn't set
	 * up */
	if (!thousands_sep_input[0]) {
		if (strcmp(decimal_pt, ".") == 0)
			thousands_sep_input = ",";
	}

	/* fetch the currency symbol.  default to '$' */
	currency = lc->currency_symbol;
	if (!currency[0]) {
		/* as above, this lets us strip '$', even if the
		 * locale isn't set up.  we don't use currency for
		 * output purposes */
		currency = "$";
	} else {
		/* make sure the currency symbol doesn't conflict with
		 * any command names, because we're going to simply
		 * delete the symbol from input lines before parsing.
		 * A few are known to match, or be substrings of, our
		 * commands.  */

		/* first check if it's unicode.  if so, no worries. */
		if (!isascii(*currency))
			return;

		/* then search for it anywhere in every command */
		oper *op = opers;
		while (op->name) {
			if (strstr(op->name, currency)) {
				currency = 0;
				break;
			}
			op++;
		}
	}

}

/* the opers[] table doesn't initialize everything explicitly */
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

// *INDENT-OFF*.
struct oper opers[] = {
//       +-------------------------------- section header if no function ptr
//       |
//       |
//       V
    {"Numerical operators with two operands:"},
//        +------------------------------- operator names
//        |    +-------------------------- function pointer
//        |    |                +--------- help (if 0, shares next cmd's help)
//        |    |                |  +------ # of operands (0 means pseudop),
//        |    |                |  |        -1 (Sym) means none (a named value)
//        |    |                |  |  +--- operator precedence
//        |    |                |  |  |         (# of operands and precedence
//        V    V                V  V  V           are used only by infix code)
	{"+", add,		0, 2, 24 },
	{"-", subtract,		"Add and subtract x and y", 2, 24 },
	{"*", multiply,		0, 2, 26 },
	{"x", multiply,		"Multiply x and y", 2, 26 },
	{"/", divide,		0, 2, 26 },
	{"%", modulo,		"Divide and modulo of y by x", 2, 26 },
	{"^", y_to_the_x,	0, 2, 28, 'R'},
	{"**", y_to_the_x,	"Raise y to the x'th power", 2, 28, 'R'},
	{">>", rshift,		0, 2, 22 },
	{"<<", lshift,		"Right/left logical shift of y by x bits", 2, 22 },
	{"&", bitwise_and,	0, 2, 20 },
	{"|", bitwise_or,	0, 2, 16 },
	{"xor", bitwise_xor,	"Bitwise AND, OR, and XOR of y and x", 2, 18 },
	{"setb", setbit,	0, 2, 16 },
	{"clearb", clearbit,	"Set and clear bit x in y", 2, 20 },
	{""},		// all-null entries cause blank line in output
    {"Numerical operators with one operand:"},
	{"~", bitwise_not,	"Bitwise NOT of x (1's complement)", 1, 30, 'R' },
	{"chs", chsign,		0, 1, 30, 'R' },
	{"negate", chsign,	"Change sign of x (2's complement)", 1, 30, 'R' },
	{"nop", nop,		"Does nothing", 1, 30, 'R' },
	{"recip", recip,	0, 1, 30, 'R' },
	{"sqrt", squarert,	"Reciprocal and square root of x", 1, 30, 'R' },
	{"sin", sine,		0, 1, 30, 'R' },
	{"cos", cosine,		0, 1, 30, 'R' },
	{"tan", tangent,	"", 1, 30, 'R' },
	{"asin", asine,		0, 1, 30, 'R' },
	{"acos", acosine,	0, 1, 30, 'R' },
	{"atan", atangent,	"Trig functions", 1, 30, 'R' },
	{"atan2", atangent2,	"Arctan of y/x (2 operands)", 2, 27 },
	{"exp", e_to_the_x,	"Raise e to the x'th power", 1, 30, 'R' },
	{"ln", log_natural,	0, 1, 30, 'R' },
	{"log2", log_base2,	0, 1, 30, 'R' },
	{"log10", log_base10,	"Natural, base 2, and base 10 logarithms", 1, 30, 'R' },

	{"abs", absolute,	0, 1, 30, 'R' },
	{"frac", fraction,	0, 1, 30, 'R' },
	{"int", integer,	"Absolute value, fractional and integer parts of x", 1, 30, 'R' },
	{""},
    {"Logical operators (mostly two operands):"},
	{"&&", logical_and,	0, 2, 10 },
	{"||", logical_or,	"Logical AND and OR", 2, 8 },
	{"==", is_eq,		0, 2, 12 },
	{"!=", is_neq,		0, 2, 12 },
	{"<", is_lt,		0, 2, 14 },
	{"<=", is_le,		0, 2, 14 },
	{">", is_gt,		0, 2, 14 },
	{">=", is_ge,		"Arithmetic comparisons", 2, 14 },
	{"!", logical_not,	"Logical NOT of x", 1, 30, 'R'},
	{""},
    {"Constants and storage:"},
	{"sto", store,		0, Auto },
	{"rcl", recall,		"Save, or push off-stack storage", Sym },
	{"pi", push_pi,		0, Sym },
	{"e", push_e,		"Push constant pi or e", Sym },
	{"lastx", repush,	0, Sym },
	{"lx", repush,		"Push previous value of x", Sym },
	{"_<name>", nop,	"Push variable" },
	{"=", assignment,	"Assign variable.  RPN: \"3 = _v\"   infix: \"(_v = 3)\"", 2, 6 },
	{"variables", showvars, 0 },
	{"vars", showvars, "Show the current list of variables" },
	{""},
    {"Unit conversions (one operand):"},
	{"i2mm", units_in_mm,	0, 1, 30, 'R' },
	{"mm2i", units_mm_in,	"inches / millimeters", 1, 30, 'R' },
	{"ft2m", units_ft_m,	0, 1, 30, 'R' },
	{"m2ft", units_m_ft,	"feet / meters", 1, 30, 'R' },
	{"mi2km", units_mi_km,	0, 1, 30, 'R' },
	{"km2mi", units_km_mi,	"miles / kilometers", 1, 30, 'R' },
	{"f2c", units_F_C,	0, 1, 30, 'R' },
	{"c2f", units_C_F,	"degrees F/C", 1, 30, 'R' },
	{"oz2g", units_oz_g,	0, 1, 30, 'R' },
	{"g2oz", units_g_oz,	"US ounces / grams", 1, 30, 'R' },
	{"oz2ml", units_oz_ml,	0, 1, 30, 'R' },
	{"ml2oz", units_ml_oz,	"US fluid ounces / milliliters", 1, 30, 'R' },
	{"q2l", units_qt_l,	0, 1, 30, 'R' },
	{"l2q", units_l_qt,	"US quarts / liters", 1, 30, 'R' },
	{"d2r", units_deg_rad,	0, 1, 30, 'R' },
	{"r2d", units_rad_deg,	"degrees / radians", 1, 30, 'R' },
	{"mpg2l100km", units_mpg_l100km, "mpg to l/100km and vice versa", 1, 30, 'R' },
	{""},
    {"Other:"},
	{"(", open_paren,	0, 0, 32 },
	{")", close_paren,	"Infix grouping", 0, 32 },
	{";", semicolon,	"Infix separator (in RPN, discards y)", 2, 4 },
	{"snapshot", snapshot,	0, Auto}, // "Snapshot the stack, stop at \"mark\" if set", Auto },
	{"sum", sum,		0, Auto },
	{"avg", avg,		"Snapshot, sum or average stack, stop at \"mark\" if set", Auto },
	{"mark", mark,		"Mark stack to limit later snap/sum/average" },
	{"restore", restore,	"Push the snapshot onto current stack", Auto },
	{""},
    {"Stack manipulation:"},
	{"clear", clear,	"Clear stack" },
	{"pop", rolldown,	"Pop (and discard) x", Auto },
	{"push", enter,		0, Auto },
	{"dup", enter,		"Push (a duplicate of) x", Auto },
	{"exch", exchange,	0, Auto },
	{"swap", exchange,	"Exchange x and y", Auto },
	{""},
    {"Display:"},
	{"P", printall,		"Print whole stack according to mode" },
	{"p", printone,		"Print x according to mode" },
	{"f", printfloat,	0 },
	{"d", printdec,		0 },
	{"u", printuns,		"Print x as float, decimal, unsigned decimal," },
	{"h", printhex,		0 },
	{"o", printoct,		0 },
	{"b", printbin,		"     hex, octal, or binary" },
	{"automatic", automatic, 0, Auto },
	{"auto", automatic,	"Select general purpose floating display format", Auto },
	{"engineering", engineering, 0, Auto },
	{"eng", engineering,	"Select engineering style floating display format", Auto },
	{"fixed", fixedpoint,	"Select fixed decimal floating display format", Auto },
	{"digits", digits,	"Number of digits for floating formats", Auto },
	{""},
    {"Modes:"},
	{"F", modefloat,	0 },
	{"D", modedec,		0 },
	{"H", modehex,		0 },
	{"O", modeoct,		0 },
	{"B", modebin,		"Switch to floating, decimal, hex, octal, binary modes" },
	{"width", width,	0, Auto },
	{"w", width,		"Set effective word size for integer modes", Auto },
	{"zerofill", zerof,	0, Auto },
	{"z", zerof,		"Toggle left-fill with zeros in H, O, and B modes", Auto },
	{"rightalign", rightalign, 0, Auto },
	{"right", rightalign,	"Toggle right alignment of numbers", Auto },
	{"degrees", use_degrees, "Toggle trig functions: degrees (1) or radians (0)" },
	{"autoprint", autop,	0 },
	{"a", autop,		"Toggle autoprinting on/off with 0/1" },
#if PRINTF_SEPARATORS
	{"separators", separators, 0 },
	{"s", separators,	"Toggle numeric separators (i.e., commas) on/off (0/1)" },
#endif
	{"mode", modeinfo,	"Display current mode parameters" },
	{""},
    {"Debug support:"},
	{"state", printstate,	"Show calculator state" },
	{"raw", printrawhex,	"Print x as raw floating hex" },
	{"Raw", moderawhex,	"Switch to raw floating hex mode"},
	{"rounding", rounding,	"Toggle snapping and rounding of floats" },
	{"tracing", tracetoggle,"Set tracing level" },
//	{"commands", commands,	"Dump raw command table" }, # use "11 tracing"
	{""},
    {"Housekeeping:"},
	{"?", help,		0 },
	{"help", help,		"Show this list (using $PAGER, if set)" },
	{"precedence", precedence, "List infix operator precedence" },
	{"quit", quit,		0 },
	{"q", quit,		0 },
	{"exit", quit,		"Leave the calculator" },
	{"errorexit", enable_errexit,	"Toggle exiting on error and warning" },
	{"license", license,	"Display the rca copyright and license." },
	{"version", version,	"Show program version" },
	{"#", help,		"Comment. The rest of the line will be ignored." },
	{""},
	{NULL, NULL, 0},
};
// *INDENT-ON*.

void
do_autoprint(token *pt)
{
	if (!autoprint)
		return;

	/* The goal is to autoprint unless it would be very redundant,
	 * or if nothing really happened.  If the user types "23", we
	 * don't want to immediately print "23".  But if they typed
	 * using a different base, or if what they typed wasn't
	 * numeric, we'll print the top of stack.  */
	switch (pt->type) {
	case OP:
		if (pt->val.oper->operands == 0)  // pseudo op
			return;
		break;

	case SYMBOLIC:
	case VARIABLE:
		break;

	case NUMERIC:
		if ((mode == 'F' || mode == 'D') &&
			pt->imode == 'D')
			return;
		if (pt->imode == mode)
			return;
		break;

	default:
		return;
	}

	if (tracing)  // try not to mix our output with debug lines
		putchar('\n');

	print_top(mode);
}

int
main(int argc, char *argv[])
{
	static token prevtok;
	token tok, *t;
	char *pn;

	pn = strrchr(argv[0], '/');
	progname = pn ? (pn + 1) : argv[0];

	/* fetch_line() will process args as if they were input as commands */
	g_argc = argc;
	g_argv = argv;

	locale_init();

	setup_width(0);
	detect_epsilon();

	create_infix_support_tokens();

	/* we simply loop forever, either pushing operands or
	 * executing operators.  the special end-of-line token lets us
	 * do reasonable autoprinting, if the last thing on the line
	 * was an operator.
	 */
	while (1) {

		/* use up tokens created by infix processing first */
		if ((t = tpop(&infix_rpn_queue))) {
			tok = *t;
			free(t);
			freeze_lastx();
		} else { /* otherwise get tokens from input as usual */
			if (!gettoken(&tok))
				continue;
			thaw_lastx();
		}
		t = &tok;

		if (t->type != EOL && t->type != OP) {
			/* don't save pending info older than one command */
			pending_clear();
		}

		switch (t->type) {
		case NUMERIC:
			result_push(t->val.val);
			break;
		case VARIABLE:
			dynamic_var(t);
			break;
		case SYMBOLIC:
		case OP:
			trace(( "invoking %s\n", t->val.oper->name));
			if (t->val.oper->func == quit)
				pending_show();
			else
				pending_clear();
			(void)(t->val.oper->func) ();
			break;
		case EOL:
			do_autoprint(&prevtok);
			pending_show();
			break;
		default:
		case UNKNOWN:
			// I think this is unreachable
			error(" error: unrecognized input '%s'\n", t->val.str);
			break;
		}
		if (variable_write_enable)
			variable_write_enable--;

		prevtok = *t;

	}
	exit(3);  // not reached
}
