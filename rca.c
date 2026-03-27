char *release = "vMP";
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
 *    The current version now uses the mpdecimal math library, so
 *    except for the trig functions (not available in mpdecimal),
 *    precision is no longer limited by the native FP hardware and API.
 *
 *
 *  If you don't have the Makefile, build with one of:
 *    gcc -o rca -D USE_EDITLINE rca.c -lmpdec -lm -ledit
 *    gcc -o rca -D USE_READLINE rca.c -lmpdec -lm -lreadline
 *    gcc -o rca rca.c -lmpdec -lm
 *	(note: don't distribute readline builds, unless you're
 *	prepared to support the GPL license requirements)
 *
 */

char licensetext[] = \
"\
 RCA License                  (SPDX-License-Identifier: BSD-2-Clause)\n\
 ------------\n\
 Copyright (C) 1993-2026  Paul Fox\n\
\n\
 Redistribution and use in source and binary forms, with or without\n\
 modification, are permitted provided that the following conditions\n\
 are met:\n\
 1. Redistributions of source code must retain the above copyright\n\
    notice, this list of conditions and the following disclaimer.\n\
 2. Redistributions in binary form must reproduce the above copyright\n\
    notice in the documentation and/or other materials provided with\n\
    the distribution.\n\
\n\
 This software is provided by the author ``as is'' and any\n\
 express or implied warranties, including, but not limited to, the\n\
 implied warranties of merchantability and fitness for a particular\n\
 purpose, are disclaimed.  In no event shall the author be liable\n\
 for any direct, indirect, incidental, special, exemplary, or\n\
 consequential damages (including, but not limited to, procurement\n\
 of substitute goods or services; loss of use, data, or profits; or\n\
 business interruption) however caused and on any theory of liability,\n\
 whether in contract, strict liability, or tort (including negligence\n\
 or otherwise) arising in any way out of the use of this software, even\n\
 if advised of the possibility of such damage.\n\
";

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

#include <mpdecimal.h>

#if defined(USE_EDITLINE)
# include <editline/readline.h>
#elif defined(USE_READLINE)
# include <readline/readline.h>
# include <readline/history.h>
#endif

#define DO_VALGRIND_CHECKS 1
#if DO_VALGRIND_CHECKS

#include <valgrind/memcheck.h>

void
valgrind(char *s)
{
	extern int tracing;
	(void)s;

	if (!RUNNING_ON_VALGRIND)
		return;

	if (tracing)
		fprintf(stderr, ",,, valgrind %s\n", s);

	/* with this, problems are reported the first time they're
	 * detected, and again in the full summary when we quit */
	VALGRIND_DO_ADDED_LEAK_CHECK;

	if (tracing)
		fprintf(stderr, ",,, valgrind %s done\n", s);
}

#else

#define valgrind(s)  do { } while(0)

#endif

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

mpd_context_t context, *ctx = &context;


/* debug logging support, runtime controllable */
int tracing;
#define TOK 1
#define EXEC 2
#define TWEAK 4
#define SHUNT 8
char *tracenames[] = {"tokens", "execution", "tweaking", "shunting", 0};

typedef int boolean;
#define TRUE 1
#define FALSE 0

typedef int opreturn;
#define GOODOP 1
#define BADOP 0

typedef long double ldouble;
typedef long long ll_t;
typedef unsigned long long ull_t;

/* global copies of main's argc/argv */
int g_argc;
char **g_argv;

/* 101 digits, not including the '.' */
char *pi_val = "3.1415926535897932384626433832795028841971693993751"
	       "058209749445923078164062862089986280348253421170679";

mpd_t *pi, *e, *zero, *one, *two, *oneeighty;

/* internal representation of operands on the stack.
 * numbers are always stored as mpdecimals, even when we're in integer
 * mode.  */
struct num {
	mpd_t *mpd;
	struct num *next;
};

/* the operand stack */
struct num *stack;
int stack_count;

/* the snapshot stack */
struct num *snapstack;

/* for command repeat, like "sum" */
int stack_mark;

/* for catching infix bugs */
int infix_stacklevel;

int infix_mode;
int debug_enabled;


/* all user input is either a number or a command operator.
 * this is how operators are looked up, by name */
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

/* values for # of operands field in opers table:
 *  1 and 2 are used verbatim as operand counts
 *  0 denotes a pseudo-op, i.e. printing, configuration, or similar
 * -1 (Sym) means a symbolic "named number", like pi, lastx, rcl
 * -2 (Auto) is a pseudo-op that wants autoprint, like "pop", "exch", "sum" */
#define Sym	-1
#define Auto	-2


/* tokens are typed -- currently numbers, operators, symbolic, and line-ends */
typedef struct token {
	int type;
	mpd_t *mpd;    /* NUMERIC: malloc'ed libmpdecimal number */
	char *valstr;  /* malloc'ed string value for NUMERIC and VARIABLE */
	oper *oper;    /* OP or SYMBOLIC points into opers table */
	char *str;     /* UNKNOWN: points to input buffer, for errors */
	int imode;	    /* input mode: if NUMERIC, how was it entered?  */
	struct token *next; /* for stacking tokens when infix processing */
	int alloced;   /* don't free this.  it wasn't alloc'ed */
} token;

/* values for token type */
#define UNKNOWN  0
#define NUMERIC  'N'
#define SYMBOLIC 'S'
#define OP       'O'
#define EOL      'E'
#define VARIABLE 'V'


/* 6 major modes:  float, decimal, hex, octal, binary, and raw float.
 * all but float and raw float are integer modes.  (raw float is a
 * debug mode:  it uses the printf %a format) */
int mode = 'F';			// 'F', 'D', 'H', 'O', 'B', 'R'
boolean floating_mode(int m) { return (m == 'F' || m == 'R'); }

/* if true, exit(4) on error, warning, or access to empty operand stack */
boolean exit_on_error = FALSE;

/* true, to copy stdin to stdout when it comes from a file or pipe */
boolean echo_enabled = FALSE;

/* if true, print the top of stack after any line that ends with an operator */
boolean autoprint = TRUE;

/* informative feedback, which is only printed if the command generating
 * it is followed by a newline */
void p_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* if this is true, and the locale provides the separator and the
 * grouping information, we will decorate numbers, like "1,333,444" */
boolean digitseparators = 1;

/* in the absence of an $RCA_DIGITS environment variable, the
 * definition of DIGITS sets the number of significant digits for the
 * calculator.
 * for now, since the trig functions still use the libm API, we match
 * the number of digits those functions can give us.
 * */
#define DIGITS   LDBL_DIG
int max_digits = DIGITS;

/* float_digits may represent either the total displayed precision, or
 * the number of digits after the decimal, depending on float_specifier.
 * it will be capped at max_digits.  */
int float_digits = 6;
char *float_specifier = "automatic"; // or "engineering" or "fixed decimal"

/* zerofill controls whether digits to the left of a value are left
 * blank, or shown as zero.  applies in hex, octal, and binary modes. */
boolean zerofill = 0;

/* rightalign controls whether, when printing, we line up least
 * significant digits (right) or most significant digits (left).  */
boolean rightalignment = 1;

/* the max number of characters needed to hold our floating point
 * values, including sign, exponent, etc, is under 50 chars.  binary
 * format takes around 80 chars for a 64 bit long long.  this constant
 * is used as the size for various holding buffers for numbers being
 * prepared for display.  */
#define TEMP_BUFSIZE 512

/* these all help limit the word size to anything we want in integer mode.  */
mpd_t *int_modulo;
int max_int_width;
int int_width;
long long int_sign_bit;
long long int_mask = ~0;
#define LONGLONG_BITS (sizeof(long long) * 8)

/* these are filled in from the locale, if possible, otherwise
 * they'll default to period, comma, and dollar-sign.  none will
 * be a null pointer after locale_init() has run. */
char *decimal_pt;   // decimal_point
size_t decimal_pt_len;
char *thousands_sep;   // digit group separator
char *grouping;    // how digits should be grouped (not always by 3!)
char *currency;   // locale currency_symbol
char *locale;	    // locale name, returned from setlocale()
char *locale_modified = "";  // indicates if we've changed the locale info

/* the most recent top-of-stack */
mpd_t *lastx;
mpd_t *frozen_lastx;

/* for store/recall */
mpd_t *offstack;

/* counting state variable, which allows variables to be read/write */
int variable_write_enable;

/* where program input is coming from currently */
static char *input_ptr = NULL;

int read_token(token *, int whichparse);
int parse_token(char *p, token *t, char **nextp, int whichparse);
void putback_token(token *t);
#define RPN 1
#define INFIX 0

typedef void (*mpd_2_op_func_t)(mpd_t *, const mpd_t *, const mpd_t *,
			mpd_context_t *);
typedef void (*mpd_1_op_func_t)(mpd_t *, const mpd_t *, mpd_context_t *);


void
mpd_stuff(void)
{

	char *rdp = getenv("RCA_DIGITS");
	if (rdp) {
		char *endp;
		long digits = strtol(rdp, &endp, 10);
		if (*endp == '\0') {
			if (digits < 2)
				digits = 2;
			if (digits > 100)
				digits = 100;
			max_digits = (int)digits;
		}
	}

	mpd_init(&context, max_digits + 2);
	context.traps = 0;

	zero = mpd_new(ctx);
	mpd_set_i64(zero, 0, ctx);

	one = mpd_new(ctx);
	mpd_set_i64(one, 1, ctx);

	two = mpd_new(ctx);
	mpd_set_i64(two, 2, ctx);

	oneeighty = mpd_new(ctx);
	mpd_set_i64(oneeighty, 180, ctx);

	e = mpd_new(ctx);
	mpd_exp(e, one, ctx);

	pi = mpd_new(ctx);
	mpd_set_string(pi, pi_val, ctx);

	lastx = mpd_new(ctx);
	mpd_copy(lastx, zero, ctx);

	frozen_lastx = mpd_new(ctx);
	mpd_copy(frozen_lastx, zero, ctx);

	offstack = mpd_new(ctx);
	mpd_copy(offstack, zero, ctx);
}

void trace_mpd(int level, char *msg, const mpd_t *t);

void
mpd_free_before_copy(mpd_t **resultp, const mpd_t *a, mpd_context_t *ctx)
{
	if (*resultp) {
	    trace_mpd(EXEC, "mpd_del'ing", *resultp);
	    mpd_del(*resultp);
	    *resultp = 0;
	}
	if (!*resultp) *resultp = mpd_new(ctx);
	trace_mpd(EXEC, "copying", a);
	mpd_copy(*resultp, a, ctx);
}

void
set_lastx(mpd_t *a)
{
	mpd_free_before_copy(&lastx, a, ctx);
}

unsigned long long
ull_from_ll(long long s)
{
    unsigned long long u;
    memcpy(&u, &s, sizeof(u));
    return u;
}

void
memory_failure(void)
{
	perror("rca: memory allocation failure");
	exit(3);
}

/* handle pointer check and exit, and use calloc() to get zero-fill. */
void *
safe_calloc(size_t size)
{
       void *p = calloc(1, size);

       if (!p) memory_failure();

       return p;
}

void trace(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void
trace(int level, const char *fmt, ...)
{
	if ((tracing & level) == 0)
		return;

	fflush(stdout);

	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

}

void
trace_mpd(int level, char *msg, const mpd_t *t)
{
	if ((tracing & level) == 0)
		return;
	fprintf(stderr, "%s: (%p) ", msg, (void*)t);
	mpd_fprint(stderr, t);
	fprintf(stderr, "\n");
}


void error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
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

/* in integer mode we maintain the illusion of integer storage. */
boolean
mpd_to_integer(mpd_t *r, mpd_t *a)
{
	mpd_t *t, *q;
	t = mpd_new(ctx);
	q = mpd_new(ctx);

	mpd_trunc(t, a, ctx); // remove fractional part
	mpd_divmod(q, r, t, int_modulo, ctx); // modulo word-length

	mpd_del(q);
	mpd_del(t);

	return (mpd_cmp(r, a, ctx) != 0);
}

unsigned long long
mpd_get_64_bits(mpd_t *n)
{
	uint32_t status;
	ull_t u;
	ll_t ln;

	/* we only use this routine if we already known the value
	 * should fit in 64 bits.  if it's negative, mpdecimal won't
	 * let us have it as unsigned.  but if it's bigger than the
	 * MAX integer, it won't let us have it as integer.  so we try
	 * both ways.  */
	status = 0;
	ln = mpd_qget_i64(n, &status);
	if (status == 0) {
		u = ull_from_ll(ln);
		return u;
	}

	status = 0;
	u = mpd_qget_u64(n, &status);
	if (status == 0)
		return u;

	error("damn\n");
	fprintf(stderr, "status is 0x%x\n", status);
	return 0;
}


void
mpush(mpd_t *a)
{
	struct num *p;

	if (!floating_mode(mode))
		mpd_to_integer(a, a);

	p = (struct num *)safe_calloc(sizeof(struct num));
	p->mpd = a;
	if (a == lastx) abort();
	p->next = stack;
	stack = p;
	stack_count++;
	trace_mpd(EXEC, "mpushed", a);
}

void
mpush_copy(mpd_t *a)
{
    mpd_t *n;

    n = mpd_new(ctx);
    mpd_copy(n, a, ctx);
    mpush(n);
}

boolean
mpeek(mpd_t **f)
{
	if (!stack)
		return FALSE;

	*f = stack->mpd;

	return TRUE;
}

boolean
mpop(mpd_t **a)
{

	struct num *p = stack;
	if (!p) {
		error(" empty stack\n");
		return FALSE;
	}
	*a = p->mpd;
	stack = p->next;
	trace_mpd(EXEC, " mpopped", p->mpd);
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
	mpd_t *m;
	uint64_t u;

	if (!mpop(&m))
		return BADOP;

	u = mpd_get_u64(m, ctx);

	if (u != 0 && u != 1) {
		mpush(m);
		error(" error: toggle commands only take 0/1 as an argument\n");
		return BADOP;
	}

	*control = (u != 0);

	if (descrip)
		p_printf(" %s %s\n", descrip, u ? yes : no);

	mpd_del(m);
	return GOODOP;
}

opreturn
infixmode(void)
{
	return toggler(&infix_mode,  "Full-time infix mode",
		"enabled", "disabled");
}

opreturn
enable_echo(void)
{
	/* no confirmation messages, which might defeat the silent purpose */
	return toggler(&echo_enabled,  0, 0, 0);
}

opreturn
enable_errexit(void)
{
	return toggler(&exit_on_error,  "Exiting on errors and warnings",
		"enabled", "disabled");
}

opreturn
debug(void)
{
	return toggler(&debug_enabled,  "Debug commands",
		"enabled", "disabled");
}

opreturn
assignment(void)
{
	/* This gets decremented with every RPN token executed.  If a
	 * variable is the very next token, we'll write to it instead
	 * of read.  */
	variable_write_enable = 2;
	trace(EXEC,( " enabling assignment\n"));
	return GOODOP;
}

/* This is poorly named.  The goal it to report whether the two
 * arguments are both finite (i.e., useful), and if not, to propagate
 * the nan, or inf, in that order, as the final result of the operation.  */
int
mpd_bothfinite(mpd_t *x, mpd_t *y)
{
	if (mpd_isfinite(y) && mpd_isfinite(x))
		return 1;

	if (mpd_isnan(y)) {
		mpush(y);
		return 0;
	} else if (mpd_isnan(x)) {
		mpush(x);
		return 0;
	}

	if (!mpd_isfinite(y)) {
		mpush(y);
		return 0;
	} else if (!mpd_isfinite(x)) {
		mpush(x);
		return 0;
	}
	return 1;
}

typedef void (*bitwise_2_op_func_t)(uint64_t *, uint64_t, uint64_t);

opreturn
bitwise_2_op_shell(char *which, bitwise_2_op_func_t f, int checkdistance)
{
	mpd_t *mx, *my;
	uint64_t x, y, r;

	if (!mpop(&mx))
		return BADOP;

	if (!mpop(&my)) {
		mpush(mx);
		return BADOP;
	}

	// if not, completes operation silently and appropriately
	if (!mpd_bothfinite(my, mx))
		return GOODOP;

	// adjusts stack and emits message on failure
	if (checkdistance && mpd_isnegative(mx)) {
		mpush(my);
		mpush(mx);
		error(" error: %s by negative not allowed\n", which);
		return BADOP;
	}

	set_lastx(mx);
	mpd_to_integer(my, my);
	mpd_to_integer(mx, mx);

	y = mpd_get_64_bits(my);
	x = mpd_get_64_bits(mx);

	f(&r, y, x);

	mpd_set_u64(my, r, ctx);
	mpd_to_integer(my, my);

	mpush(my);
	mpd_del(mx);

	return GOODOP;
}

typedef void (*bitwise_1_op_func_t)(uint64_t *, uint64_t);

opreturn
bitwise_1_op_shell(bitwise_1_op_func_t f)
{
	mpd_t *mx;
	uint64_t x, r;

	if (!mpop(&mx))
		return BADOP;

	if (!mpd_isfinite(mx)) {
		mpush(mx);
		return GOODOP;
	}

	set_lastx(mx);
	mpd_to_integer(mx, mx);

	x = mpd_get_64_bits(mx);

	f(&r, x);

	mpd_set_u64(mx, r, ctx);
	mpd_to_integer(mx, mx);

	mpush(mx);

	return GOODOP;
}

opreturn
mpd_2_op_shell(mpd_2_op_func_t f)
{
	mpd_t *x, *y;

	if (!mpop(&x))
		return BADOP;

	if (!mpop(&y)) {
		mpush(x);
		return BADOP;
	}

	set_lastx(x);
	f(y, y, x, ctx);
	if (!floating_mode(mode))
		mpd_to_integer(y, y);

	mpd_del(x);
	mpush(y);

	return GOODOP;
}

opreturn
mpd_1_op_shell(mpd_1_op_func_t f)
{
	mpd_t *x;
	if (!mpop(&x))
		return BADOP;

	set_lastx(x);
	f(x, x, ctx);
	if (!floating_mode(mode))
		mpd_to_integer(x, x);

	mpush(x);

	return GOODOP;
}

opreturn
add(void)
{
	return mpd_2_op_shell(mpd_add);
}

opreturn
subtract(void)
{
	return mpd_2_op_shell(mpd_sub);
}


opreturn
multiply(void)
{
	return mpd_2_op_shell(mpd_mul);
}

opreturn
divide(void)
{
	return mpd_2_op_shell(mpd_div);
}

void mpd_mod(mpd_t *m, const mpd_t *y, const mpd_t *x, mpd_context_t *ctx)
{
	trace_mpd(EXEC, "y is ", y);
	trace_mpd(EXEC, "x is ", x);

	mpd_t *q = mpd_new(ctx);
	mpd_t *t = mpd_new(ctx);

	mpd_div(q, y, x, ctx);  // divide
	mpd_trunc(t, q, ctx);   // truncate
	mpd_mul(t, t, x, ctx);  // multiply the truncated value by divisor
	mpd_sub(m, y, t, ctx);  // subtract from original dividend

	mpd_del(t);
	mpd_del(q);
}

opreturn
modulo(void)
{
	return mpd_2_op_shell(mpd_mod);
}

opreturn
y_to_the_x(void)
{
	return mpd_2_op_shell(mpd_pow);
}

opreturn
e_to_the_x(void)
{
	return mpd_1_op_shell(mpd_exp);
}


int
bitwise_width(void)
{
	return floating_mode(mode) ? 64 : int_width;
}

void
rshift_worker(uint64_t *r, uint64_t y, uint64_t x)
{
	if (x >= (uint64_t)bitwise_width()) {
		*r = 0;
	} else {
		uint64_t i = x;
		uint64_t j = y;

		if (i > (uint64_t)int_width)
			i = (uint64_t)int_width;
		while (i--) {
			j = (j >> 1UL); // & ~(uint64_t)int_sign_bit;
		}
		*r = j;
	}

}

opreturn
rshift(void)
{
	return bitwise_2_op_shell("shift", rshift_worker, 1);
}

void
lshift_worker(uint64_t *r, uint64_t y, uint64_t x)
{
	if (x >= (uint64_t)bitwise_width()) {
		*r = 0;
	} else {
		*r = y << x;
	}
}
opreturn
lshift(void)
{
	return bitwise_2_op_shell("shift", lshift_worker, 1);
}

void
ror_worker(uint64_t *r, uint64_t y, uint64_t x)
{
	int64_t i = (int64_t)x;
	int64_t j = (int64_t)y;

	i %= bitwise_width();
	while (i--) {
		long long rbit = (j & 1);
		j = (((j >> 1) & ~int_sign_bit) | (rbit << (int_width - 1)));
	}
	*r = (uint64_t)j;
}

opreturn
rotateright(void)
{
	return bitwise_2_op_shell("rotate", ror_worker, 1);
}

void
rol_worker(uint64_t *r, uint64_t y, uint64_t x)
{
	int64_t i = (int64_t)x;
	int64_t j = (int64_t)y;

	i %= bitwise_width();
	while (i--) {
		long long rbit = (j & int_sign_bit);
		j = (((j << 1) & ~1) | (rbit != 0));
	}
	*r = (uint64_t)j;
}

opreturn
rotateleft(void)
{
	return bitwise_2_op_shell("rotate", rol_worker, 1);
}

void
bitwise_and_worker(uint64_t *r, uint64_t y, uint64_t x)
{
	*r = x & y;
}

opreturn
bitwise_and(void)
{
	return bitwise_2_op_shell("and", bitwise_and_worker, 0);
}

void
bitwise_or_worker(uint64_t *r, uint64_t y, uint64_t x)
{
	*r = x | y;
}

opreturn
bitwise_or(void)
{
	return bitwise_2_op_shell("or", bitwise_or_worker, 0);
}

void
bitwise_xor_worker(uint64_t *r, uint64_t y, uint64_t x)
{
	*r = x ^ y;
}

opreturn
bitwise_xor(void)
{
	return bitwise_2_op_shell("xor", bitwise_xor_worker, 0);
}

void
setbit_worker(uint64_t *r, uint64_t y, uint64_t x)
{
	uint64_t i = x, j = y;

	if (i < (uint64_t)bitwise_width())
		j |= (1LL << i);

	*r = j;
}

opreturn
setbit(void)
{
	return bitwise_2_op_shell("set bit", setbit_worker, 1);
}

void
clearbit_worker(uint64_t *r, uint64_t y, uint64_t x)
{
	uint64_t i = x, j = y;

	if (i < (uint64_t)bitwise_width())
		j &= ~(1ULL << i);

	*r = j;
}

opreturn
clearbit(void)
{
	return bitwise_2_op_shell("clear bit", clearbit_worker, 1);
}

void
bitwise_not_worker(uint64_t *r, uint64_t x)
{
	uint64_t i = x;

	*r = ~i;
}

opreturn
bitwise_not(void)
{
	return bitwise_1_op_shell(bitwise_not_worker);
}

void
bitcount_worker(uint64_t *r, uint64_t x)
{
	uint64_t i = 0;

	if (!floating_mode(mode))
		x &= (uint64_t)int_mask;

	/*
	 * I wasn't going to include a bitcount operator, until I came
	 * across this, suggested by "the bible" (C Programming Language 2nd
	 * Ed.), and often credited to Brian Kernighan.  The book makes no
	 * such claim, and he later pointed out that it was first published
	 * by Peter Wegner, in CACM 3 (1960), 322.  It's not the fastest bit
	 * counting technique, but very clever and very simple.
	 */
	while (x > 0) {
		x &= x - 1;  // always clears the least significant 1
		i++;
	}
	*r = i;
}

opreturn
bitcount(void)
{
	return bitwise_1_op_shell(bitcount_worker);
}

opreturn
chsign(void)
{
	return mpd_1_op_shell(mpd_copy_negate);
}


opreturn
nop(void)
{
	return GOODOP;
}

opreturn
rpnswitch(void)
{
	return GOODOP;
}

opreturn
absolute(void)
{
	return mpd_1_op_shell(mpd_copy_abs);
}

opreturn
recip(void)
{
	mpd_t *x;

	if (!mpop(&x))
		return BADOP;

	set_lastx(x);
	mpd_div(x, one, x, ctx);
	mpush(x);
	return GOODOP;
}

opreturn
squarert(void)
{
	return mpd_1_op_shell(mpd_sqrt);
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

void
mpd_radians_to_degrees(mpd_t *r, mpd_t *a)
{
	// return (angle * 180.0L) / pi;
	mpd_mul(a, a, oneeighty, ctx);
	mpd_div(r, a, pi, ctx);
}

void
mpd_degrees_to_radians(mpd_t *r, mpd_t *a)
{
	// return (angle * pi) / 180.0L;
	mpd_mul(a, a, pi, ctx);
	mpd_div(r, a, oneeighty, ctx);
}

void
mpd_radians_to_user_angle(mpd_t *user, mpd_t *rads)
{
	if (trig_degrees)
		mpd_radians_to_degrees(user, rads);
	else
		mpd_copy(user, rads, ctx);
}

void
mpd_user_angle_to_radians(mpd_t *rads, mpd_t *user)
{
	if (trig_degrees)
		mpd_degrees_to_radians(rads, user);
	else
		mpd_copy(rads, user, ctx);
}

void
mpd_user_angle_to_degrees(mpd_t *degrees, mpd_t *user)
{
	if (trig_degrees)
		mpd_copy(degrees, user, ctx);
	else
		mpd_radians_to_degrees(degrees, user);
}

int
mpd_to_double(ldouble *dp, mpd_t *m)
{
	char fmt[32];
	char *s, *es;

	snprintf(fmt, sizeof fmt, ".%dg", max_digits);
	s = mpd_format(m, fmt, ctx);

	errno = 0;
	*dp = strtold(s, &es);

	int ok = ((*es == '\0') && (errno == 0));

	free(s);

	return ok;
}

void
mpd_from_double(mpd_t *m, ldouble d, mpd_context_t *ctx)
{
	char buf[128];
	snprintf(buf, sizeof(buf), "%.*Lg", LDBL_DECIMAL_DIG, d);
	mpd_set_string(m, buf, ctx);
}

#define SIN 1
#define COS 2
#define TAN 3
#define aSIN 4
#define aCOS 5
#define aTAN 6

opreturn
trig(int which)
{
	mpd_t *mx;
	ldouble dx;

	if (!floating_mode(mode))
		return trig_no_sense();

	if (!mpop(&mx))
		return BADOP;

	set_lastx(mx);
	if (which <= TAN)
		mpd_user_angle_to_radians(mx, mx);

	if (!mpd_to_double(&dx, mx)) {
		error("error: trig input angle likely out of range\n");
		mpd_del(mx);
		return BADOP;
	}

	switch (which) {
	case SIN:  dx = sinl(dx); break;
	case COS:  dx = cosl(dx); break;
	case TAN:  dx = tanl(dx); break;
	case aSIN:  dx = asinl(dx); break;
	case aCOS:  dx = acosl(dx); break;
	case aTAN:  dx = atanl(dx); break;
	}

	mpd_from_double(mx, dx, ctx);

	if (which >= aSIN)
		mpd_radians_to_user_angle(mx, mx);

	mpush(mx);

	return GOODOP;
}

opreturn
sine(void)
{
	return trig(SIN);
}

opreturn
cosine(void)
{
	return trig(COS);
}

opreturn
tangent(void)
{
	return trig(TAN);
}

opreturn
asine(void)
{
	return trig(aSIN);
}

opreturn
acosine(void)
{
	return trig(aCOS);
}

opreturn
atangent(void)
{
	return trig(aTAN);
}

opreturn
atangent2(void)
{
	mpd_t *mx, *my;
	ldouble dx, dy, r;

	if (!floating_mode(mode))
		return trig_no_sense();

	if (mpop(&mx)) {
		if (mpop(&my)) {
			if (!mpd_to_double(&dx, mx) ||
				!mpd_to_double(&dy, my)) {
				error("error: trig input likely out of range");
				mpd_del(mx);
				mpd_del(my);
				return BADOP;
			}
			r =  atan2l(dy, dx);
			set_lastx(mx);
			mpd_from_double(mx, r, ctx);
			mpd_radians_to_user_angle(mx, mx);
			mpush(mx);
			mpd_del(my);
			return GOODOP;
		}
		mpush(mx);
	}
	return BADOP;
}

opreturn
log_base2(void)
{
	mpd_t *x;
	static mpd_t *ln2;

	if (!ln2) { // initialization
		ln2 = mpd_new(ctx);
		mpd_add(ln2, one, one, ctx);
		mpd_ln(ln2, ln2, ctx);
	}

	if (!mpop(&x))
		return BADOP;

	set_lastx(x);
	mpd_ln(x, x, ctx);
	mpd_div(x, x, ln2, ctx);
	mpush(x);

	return GOODOP;
}

opreturn
log_natural(void)
{
	return mpd_1_op_shell(mpd_ln);
}

opreturn
log_base10(void)
{
	return mpd_1_op_shell(mpd_log10);
}

opreturn
fraction(void)
{
	mpd_t *x, *t;
	if (!mpop(&x))
		return BADOP;

	t = mpd_new(ctx);

	set_lastx(x);
	mpd_trunc(t, x, ctx);
	mpd_sub(x, x, t, ctx);
	mpush(x);
	mpd_del(t);

	return GOODOP;
}

opreturn
integer(void)
{
	return mpd_1_op_shell(mpd_trunc);
}

opreturn
logical_compare_worker(int *xz, int *yz)
{
	mpd_t *x, *y;

	if (!mpop(&x)) {
		return BADOP;
	}
	if (!mpop(&y)) {
		mpush(x);
		return BADOP;
	}

	set_lastx(x);

	*xz = mpd_iszero(x);
	*yz = mpd_iszero(y);

	mpd_del(x);
	mpd_del(y);

	return GOODOP;
}

opreturn
logical_and(void)
{
	int xz, yz;

	if (logical_compare_worker(&xz, &yz) != GOODOP)
		return BADOP;

	mpush_copy((!xz && !yz) ? one : zero);

	return GOODOP;
}

opreturn
logical_or(void)
{
	int xz, yz;

	if (logical_compare_worker(&xz, &yz) != GOODOP)
		return BADOP;

	mpush_copy((!xz || !yz) ? one : zero);

	return GOODOP;
}

#define EQ  1
#define NEQ 2
#define LT  3
#define LE  4
#define GT  5
#define GE  6

opreturn
compare_worker(int c)
{
	mpd_t *x, *y;

	if (!mpop(&x)) {
		return BADOP;
	}
	if (!mpop(&y)) {
		mpush(x);
		return BADOP;
	}

	set_lastx(x);

	mpd_rescale(x, x, -max_digits, ctx);
	mpd_rescale(y, y, -max_digits, ctx);

	int r = mpd_cmp(y, x, ctx);
	switch(c) {
	case EQ:  mpush_copy((r == 0) ? one : zero); break;
	case NEQ: mpush_copy((r != 0) ? one : zero); break;
	case LT:  mpush_copy((r < 0) ? one : zero); break;
	case LE:  mpush_copy((r <= 0) ? one : zero); break;
	case GT:  mpush_copy((r > 0) ? one : zero); break;
	case GE:  mpush_copy((r >= 0) ? one : zero); break;
	}

	mpd_del(x);
	mpd_del(y);

	return GOODOP;
}

opreturn
is_eq(void)
{
	return compare_worker(EQ);
}

opreturn
is_neq(void)
{
	return compare_worker(NEQ);
}

opreturn
is_lt(void)
{
	return compare_worker(LT);
}

opreturn
is_le(void)
{
	return compare_worker(LE);
}

opreturn
is_gt(void)
{
	return compare_worker(GT);
}

opreturn
is_ge(void)
{
	return compare_worker(GE);
}

opreturn
logical_not(void)
{
	mpd_t *x;

	if (!mpop(&x))
		return BADOP;

	set_lastx(x);

	mpush_copy( mpd_iszero(x) ? one : zero);

	mpd_del(x);

	return GOODOP;
}

opreturn
clear(void)
{
	mpd_t *x;

	if (!mpeek(&x))
		return GOODOP;

	set_lastx(x);

	while(stack) {
		mpop(&x);
		mpd_del(x);
	}

	return GOODOP;
}

opreturn
rolldown(void)			// aka "pop"
{
	mpd_t *b;
	if (!mpop(&b))
		return BADOP;
	mpd_free_before_copy(&lastx, b, ctx);
	mpd_del(b);
	return GOODOP;
}

opreturn
enter(void)
{
	mpd_t *x;

	if (mpop(&x)) {
		mpush(x);
		mpd_t *n = mpd_new(ctx);
		mpd_copy(n, x, ctx);
		mpush(n);
		return GOODOP;
	}
	return BADOP;
}

// during an infix evaluation, lastx needs to kept at its pre-infix value.
boolean lastx_is_frozen = 0;

void
freeze_lastx(void)
{
	if (!lastx_is_frozen) {
		mpd_t *x;
		if (mpeek(&x)) {
			mpd_free_before_copy(&frozen_lastx, x, ctx);
		} else {
			if (frozen_lastx)
				mpd_del(frozen_lastx);
			frozen_lastx = 0;
		}
		lastx_is_frozen = TRUE;
		infix_stacklevel = stack_count;
	}
}

void
thaw_lastx(void)
{
	if (lastx_is_frozen) {
		lastx_is_frozen = FALSE;
		if (frozen_lastx) {
			set_lastx(frozen_lastx);
			mpd_del(frozen_lastx);
		}
		frozen_lastx = 0;
		// infix must add either no or 1 value to the stack
		int i = stack_count - infix_stacklevel;
		if (i != 0 && i != 1)
			error("BUG: stack changed by %d after infix\n",
				stack_count - infix_stacklevel);
		infix_stacklevel = -1;
	}
}

opreturn
repush(void)			// aka "lastx"
{
	if (lastx_is_frozen)
		mpush_copy(frozen_lastx);
	else
		mpush_copy(lastx);

	return GOODOP;
}

opreturn
exchange(void)
{
	mpd_t *x, *y;

	if (mpop(&y)) {
		if (mpop(&x)) {
			mpush(y);
			mpush(x);
			return GOODOP;
		}
		mpush(y);
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

void
memfile_close(struct memfile *mfp)
{
	fflush(mfp->fp);
	fclose(mfp->fp);
	free(mfp->bufp);
	mfp->fp = 0;
}

struct memfile pp;

int pending_enabled = 1;

void
pending_suppress(void)
{
	pending_enabled = 0;
}

void
pending_allow(void)
{
	pending_enabled = 1;
}

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
	if (pp.fp && pending_enabled) {
		fflush(pp.fp);
		printf("%s", pp.bufp);
		pending_clear();
	}
}

void
p_printf(const char *fmt, ...)  // short for pending_printf()
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
putbinary(unsigned long long u)
{
	int i;
	int zf = zerofill; // leading_zeros;

	u &= (ull_t)int_mask;

	m_file_start();

	fprintf(mp.fp, " 0b");
	for (i = int_width-1; i >= 0; i--) {
		if (u & (1L << i)) {
			fputc('1', mp.fp);
			zf = 1;
		} else if (zf || i == 0) {
			fputc('0', mp.fp);
		}
		if (i && (i % 8 == 0)) {
			if (digitseparators && zf)
				fputs(thousands_sep, mp.fp);
		}
	}

	m_file_finish();

	return mp.bufp;
}

char *
puthex(unsigned long long u)
{
	int i;
	int nibbles = ((int_width + 3) / 4);
	int zf = zerofill; // leading_zeros;

	u &= (ull_t)int_mask;

	m_file_start();

	fprintf(mp.fp," 0x");
	for (i = nibbles-1; i >= 0; i--) {
		int nibble = (u >> (4 * i)) & 0xf;
		if (nibble || zf || i == 0) {
		    fputc("0123456789abcdef"[nibble], mp.fp);
		    zf = 1;
		}
		if (i && (i % 4 == 0)) {
		    if (digitseparators && zf)
			    fputs(thousands_sep, mp.fp);
		}
	}

	m_file_finish();

	return mp.bufp;
}

char *
putoct(unsigned long long u)
{
	int i;
	int triplets = ((int_width + 2) / 3);
	int zf = zerofill; // leading_zeros;

	u &= (ull_t)int_mask;

	m_file_start();

	fprintf(mp.fp," 0o");
	for (i = triplets-1; i >= 0; i--) {
		int triplet = (u >> (3 * i)) & 7;
		if (triplet || zf || i == 0) {
		    fputc("01234567"[triplet], mp.fp);
		    zf = 1;
		}
		if (i && (i % 3 == 0)) {
		    if (digitseparators && zf)
			    fputs(thousands_sep, mp.fp);
		}
	}

	m_file_finish();

	return mp.bufp;
}

void
strreverse(char *s)
{
        if (!s) return;

        char *e = s + strlen(s) - 1;
        char t;

        while (s < e) {
                t = *s; *s = *e; *e = t;    // swap start and end
                s++; e--;		    // move pointers inward
        }
}

/* take the printf'd number in iobuf, and replace it with a grouped
 * version of the same string, if configured to do so.  preserves
 * leading and trailing whitespace. */
void
add_digit_grouping(char *iobuf)
{
	char *upto;
	char *ioptr = iobuf, *tptr;
	long count;
	static char rev_dec_pt[10];
	static char *tbuf;

	if (!digitseparators)
		return;

	if (!tbuf) tbuf = safe_calloc(TEMP_BUFSIZE);

	tptr = tbuf;

	strreverse(iobuf);

	/* reverse the (potentially) multi-byte decimal point, so
	 * that we can search for it in the reversed input buffer */
	if (!rev_dec_pt[0]) {
		if (decimal_pt_len > sizeof(rev_dec_pt) - 1)
			error(" BUG: see add_digit_grouping()\n");
		strncpy(rev_dec_pt, decimal_pt, sizeof(rev_dec_pt) - 1);
		rev_dec_pt[sizeof(rev_dec_pt) - 1] = '\0';
		strreverse(rev_dec_pt);
	}

	/* working from the tail end of the number (now the front of
	 * the string), we want to copy verbatim up to the decimal
	 * point, or the e, if either exists.  */
	upto = strstr(iobuf, rev_dec_pt);
	if (upto) upto += decimal_pt_len - 1;  // point at end of rev_dec_pt
	if (!upto)
		upto = strchr(iobuf, 'e');
	if (!upto)
		upto = strchr(iobuf, 'E');

	while (isspace(*ioptr))
		*tptr++ = *ioptr++;
	if (upto) {
		count = upto - ioptr + 1;
		while (count--)
			*tptr++ = *ioptr++;
	}

	int gindex = 0;
	int gsize = grouping[gindex++];

	while (*ioptr) {
		int i;

		/* copy the group */
		for (i = gsize; i && *ioptr; i--) {
			*tptr++ = *ioptr++;
		}
		/* add the separator if there's more to come */
		if (*ioptr && isdigit(*ioptr)) {
			char *t = thousands_sep;
			while (*t)
				*tptr++ = *t++;
		}
		*tptr = '\0';

		if (grouping[gindex] == CHAR_MAX) {
			/* no further grouping */
			while (*ioptr)
				*tptr++ = *ioptr++;
			*tptr = '\0';
			break;
		}

		if (grouping[gindex] > 0)
			gsize = grouping[gindex++];

	}
	strreverse(tbuf);
	strcpy(iobuf, tbuf);
}

char *
putunsigned(unsigned long long u)
{
	char buf[TEMP_BUFSIZE];
	m_file_start();

	u &= (ull_t)int_mask;

	trace(EXEC, "putunsigned: hex is 0x%llx\n", u);

	snprintf(buf, sizeof(buf), " %llu", u);
	add_digit_grouping(buf);
	fputs(buf, mp.fp);

	m_file_finish();

	return mp.bufp;
}

char *
putsigned(long long ln)
{
	char buf[TEMP_BUFSIZE];
	m_file_start();

	snprintf(buf, sizeof(buf), " %lld", ln);
	add_digit_grouping(buf);
	fputs(buf, mp.fp);

	m_file_finish();

	return mp.bufp;
}


void
show_int_truncation(boolean changed, mpd_t *old, char *mark)
{
	if (!changed) {
		p_printf("%s\n", mark);
		return;
	}

	// don't bother showing mark if we're warning about truncation

	pending_show();
	if (floating_mode(mode)) {
		error("     # warning: format loses accuracy\n");
	} else {
		/* this prints all DIGITS+2 digits (not limited to
		 * max_digits), so user can copy/paste the full precision. */
		char *s = mpd_to_sci(old, 0);
		error("     # was %s\n", s);
		free(s);
	}
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

/* adjust the %e (scientific) format string to put it in engineering
 * format, where the exponent of 10 is always a multiple of 3 */
int
convert_eng_format(char *buf)
{
	char *p, *odp, *f, *ep, *dp;
	long exp, nexp, shift;

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
	sprintf(ep, "%02ld", labs(nexp));

	return 1;
}

void
trim_g_trailing_zeros(char *s)
{
        char *e = strchr(s, 'e');
        if (!e)
                e = strchr(s, 'E');

        /* isolate mantissa */
        char *end = e ? e : s + strlen(s);

        /* find decimal point */
        char *dot = strchr(s, '.');
        if (!dot)
                return;

        /* walk backwards trimming zeros */
        char *p = end - 1;
        while (p > dot && *p == '0')
                p--;

        /* if we stopped at the dot, remove it too */
        if (p == dot)
                p--;

        /* shift remainder (including exponent) left */
        memmove(p + 1, end, strlen(end) + 1);
}

void
zero_pad_exponent(char *s)
{
	char *e = strchr(s, 'e');

	if (!e) e = strchr(s, 'E');
	if (!e) return;

	char *p = e + 1;

	/* optional sign */
	if (*p == '+' || *p == '-')
		p++;

	/* count digits */
	char *digits = p;
	size_t len = 0;

	while (isdigit((unsigned char)*p)) {
		len++;
		p++;
	}

	if (len >= 2)
		return;

	/* shift right to make room for leading zero */
	memmove(digits + 1, digits, len + 1);
	digits[0] = '0';
}

char *
print_floating(mpd_t *m)
{
	char fmt[30];
	char buf[TEMP_BUFSIZE];

	m_file_start();
	fputc(' ', mp.fp);

	if (!mpd_isfinite(m)) {
		/* I just prefer the libc "nan" and "inf" to the mixed
		 * case "NaN" and "Infinity" strings mpdecimal provides */
		char *s; // = mpd_to_sci(m, 0);
		if (m->flags & (MPD_INF)) {
			if (m->flags & MPD_POS)	s = "+inf" ;
			else if (m->flags & MPD_NEG) s = "-inf";
			else s = "inf";
		} else if (m->flags & (MPD_NAN|MPD_SNAN)) {
			if (m->flags & MPD_POS)	s = "+nan" ;
			else if (m->flags & MPD_NEG) s = "-nan";
			else s = "nan";
		}
		fprintf(mp.fp, "%s", s);
	} else if (mpd_iszero(m)) {
		fputs("0", mp.fp);
	} else if (float_specifier[0] == 'a') { // 'a'uto

		int precision = (float_digits < 1) ? 1 : float_digits;
		int exp = (int)mpd_adjexp(m);

		/* we jump through hoops to get our output to look
		 * like printf %g output, which uses some nice
		 * heuristics to keep numbers looking familiar (i.e.,
		 * non-exponential) as much as possible.  */
		if (exp >= -4 && exp < precision) { /* fixed */
			int frac = precision - (exp + 1);
			if (frac < 0) frac = 0;
			snprintf(fmt, sizeof fmt, ".%df", frac);

		} else { /* scientific */
			snprintf(fmt, sizeof fmt, ".%de", precision - 1);
		}

		char *s = mpd_format(m, fmt, ctx);
		snprintf(buf, sizeof(buf), "%s", s);
		free(s);

		trim_g_trailing_zeros(buf);
		zero_pad_exponent(buf);
		add_digit_grouping(buf);
		fputs(buf, mp.fp);

	} else if (float_specifier[0] == 'f') { // 'f'ixed

		// first construct the format string
		snprintf(fmt, sizeof fmt, ".%df", float_digits);

		// use it to get fixed notation
		char *s = mpd_format(m, fmt, ctx);
		snprintf(buf, sizeof(buf), "%s", s);
		free(s);

		add_digit_grouping(buf);
		fputs(buf, mp.fp);

	} else if (float_specifier[0] == 'e') { // "eng"

		// first construct the format string
		int fdigs = (float_digits < 3) ? 3 : float_digits;
		snprintf(fmt, sizeof fmt, ".%de", fdigs - 1);

		// use it to get scientific notation
		char *s = mpd_format(m, fmt, ctx);
		snprintf(buf, sizeof(buf), "%s", s);
		free(s);

		// convert it to engineering format
		if (!convert_eng_format(buf)) {
			error(" BUG: parse error in engineering format\n");
		} else {
			add_digit_grouping(buf);
			fputs(buf, mp.fp);
		}
	}

	m_file_finish();

	return mp.bufp;
}

/* Previously these right-alignment columns were dynamic, adjusting to
 * the max width of the current display format and base, but it added
 * more complexity than value.  Now there are only two choices, which
 * respectively just fit the two longest formats: binary, and 64 bit octal. */
#define ALIGN_COL        32
#define ALIGN_COL_BINARY 75

int
calc_align(int binary)
{
	if (!rightalignment)
		return 0;

	if (binary)
		return ALIGN_COL_BINARY;
	else
		return ALIGN_COL;
}

int
floating_alignment(char *s)
{
	int align = 0;
	if (rightalignment) {
		char *eos, *dp;
		align = calc_align(0);
		dp = strstr(s, decimal_pt);
		if (dp) {
			eos = s + strlen(s);
			align += (int)(eos - dp);
		}
	}
	return align;
}

void
print_n(mpd_t *m, int format, boolean conv, char *mark)
{
	int64_t ln;
	uint64_t u;
	int align;
	boolean changed;

	if (!mark) mark = "";

	if (!mpd_isfinite(m) || floating_mode(format)) {
		char *pf;
		pf = print_floating(m);
		align = floating_alignment(pf);
		p_printf("%*s%s\n", align, pf, mark);
		return;
	}

	/* otherwise, if we're in an integer mode, or printing in an
	 * integer format, then we honor the current no. of bits (i.e.,
	 * integer width, even for floating point display format. */

	/* this is a little messy.  being in integer mode means the
	 * values need to be converted to integer.  (duh.) we do that
	 * here, even though this is the general "print a number"
	 * routine.  this is because it's called at the deep end of a
	 * recursive loop when printing a stack, so it also gets
	 * saddled for doing float to int conversion of values on the
	 * stack.  the conversion needs to happen when switching from
	 * float mode to an integer mode:  values need to be masked
	 * and sign extended (if word length is less than native), and
	 * we need to do it here so we can print a message about the
	 * conversion alongside the converted value.  */

	mpd_t *n = mpd_new(ctx);
	switch (format) {
	case 'H':
		changed = mpd_to_integer(n, m);
		u = mpd_get_64_bits(n);
		align = calc_align(0);
		p_printf("%*s", align, puthex(u));
		break;
	case 'O':
		changed = mpd_to_integer(n, m);
		u = mpd_get_64_bits(n);
		align = calc_align(0);
		p_printf("%*s", align, putoct(u));
		break;
	case 'B':
		changed = mpd_to_integer(n, m);
		u = mpd_get_64_bits(n);
		align = calc_align(0);
		p_printf("%*s", align, putbinary(u));
		break;
	case 'U':
		changed = mpd_to_integer(n, m);
		u = mpd_get_64_bits(n);
		align = calc_align(0);
		p_printf("%*s", align, putunsigned(u));
		break;
	case 'D':
	case 'F':
		changed = mpd_to_integer(n, m);
		u = mpd_get_64_bits(n);
		align = calc_align(0);
		/* shenanigans to make pos/neg numbers appear
		 * properly.  our masked/shortened numbers
		 * don't appear as negative to printf, so we
		 * find the reduced-width sign bit, and fake
		 * it.
		 */

		// long long mask;
		ln = (ll_t)u;
		// mask gives us everything but the sign bit
		// mask = (long long)int_mask & ~int_sign_bit;
		if (ln & int_sign_bit) {	// it's negative
			ln |= ~int_mask;
		} else {
			ln &= int_mask;
		}
		if (format == 'D')
			p_printf("%*s", align, putsigned(ln));
		else {
			char *pf;
			mpd_set_i64(n, ln, ctx);

			pf = print_floating(m);
			align = floating_alignment(pf);
			p_printf("%*s%s\n", align, pf, mark);
		}
		break;
	default:
		error(" bug: default case in print_n()\n");
		return;
	}

	// p_printf("%s\n", mark);
	show_int_truncation(changed, m, mark);
	if (changed && conv)
		mpd_copy(m, n, ctx);
	mpd_del(n);

}

void
print_top(int format)
{
	if (stack)
		print_n(stack->mpd, format, 0, 0);
}

void
printstack_worker(int n, boolean conv, struct num *s)
{

	if (s->next)
		printstack_worker(n-1, conv, s->next);

	print_n(s->mpd, mode, conv,
		(n == stack_mark) ? "         # <-  mark" : "");
}

void
printstack(boolean conv, struct num *s)
{
	if (!s)
		return;

	printstack_worker(stack_count, conv, s);
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

opreturn
printstate(void)
{

	p_printf("\n");
	p_printf(" Current mode is %c (%s)\n", mode,
			floating_mode(mode) ? "floating" : "integer" );
	p_printf("  - when in floating mode,");
	p_printf(" display is \"%s\", with %d digits\n",
		float_specifier, float_digits );

	p_printf("  - when in integer modes,");
	p_printf(" word width is %d bits\n", int_width);
	p_printf("    mask: %s", puthex((ull_t)int_mask));
	p_printf("  sign bit: %s\n", puthex((ull_t)int_sign_bit) );
	p_printf("    max integer width is %d bits\n", max_int_width);

	p_printf("\n");
	p_printf(" Locale elements, from locale '%s'%s:\n",
		locale, locale_modified);
	p_printf("  decimal '%s', thousands separator '%s', currency '%s'\n",
		decimal_pt[0] ? decimal_pt : "<none>",
		thousands_sep[0]? thousands_sep : "<none>",
		currency[0] ? currency : "<none>");
	p_printf("\n");

	p_printf(" rca descriptor: fmp%di%u\n", max_digits, max_int_width);

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
		if (float_specifier[0] == 'f') { // 'f'ixed
			/* float_digits == 7 gives:  123.4560000  */
			p_printf(" Showing %u digits after the decimal"
				" in %s format.\n",
				float_digits, float_specifier);
		} else {	/* 'a'uto..., 'e'ng... */
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
	if (!thousands_sep[0] || grouping[0] == CHAR_MAX) {
		mpd_t *discard;
		mpop(&discard);
		mpd_del(discard);
		p_printf(" No separator support in locale, "
			"numeric separators are disabled\n");
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
	float_specifier = "fixed";
	float_mode_messages(1);

	return GOODOP;
}

opreturn
digits(void)
{
	mpd_t *m;
	int i;

	if (!mpop(&m))
		return BADOP;

	i = (int)mpd_get_u32(m, ctx);
	mpd_del(m);

	char *limited = "";

	// but it can't be greater than our maximum precision
	if (i > max_digits || i < 0) {
		i = max_digits;
		limited = "the maximum of ";
	}

	// the 3 formats (auto/fixed/eng) may set their own mimimums
	float_digits = i;

	p_printf(" Floating formats configured for %s%d digit%s.\n", limited,
		float_digits, float_digits == 1 ? "" : "s");

	float_mode_messages(0);

	return GOODOP;
}

void
setup_width(int bits)
{
	if (!bits || !max_int_width) {	/* first call */
		max_int_width = LONGLONG_BITS;
		bits = max_int_width;
		int_modulo = mpd_new(ctx);

	}

	// int_modulo used as tmp var here
	mpd_set_i64(int_modulo, bits, ctx);
	mpd_pow(int_modulo, two, int_modulo, ctx);   // 2 ^ (bits+1)

	int_width = bits;
	int_sign_bit = (1LL << (int_width - 1));

	if (int_width == LONGLONG_BITS) {
		int_mask = ~0;
	} else {
		int_mask = (1LL << int_width) - 1;
	}
}

opreturn
width(void)
{
	mpd_t *mbits;
	int bits;

	if (!mpop(&mbits))
		return BADOP;

	bits = (int)mpd_get_u32(mbits, ctx);

	if (bits == -1) {
		bits = max_int_width;
	} else if (bits > max_int_width) {
		bits = max_int_width;
		p_printf(" Width out of range, set to max (%d)\n", bits);
	} else if (bits < 2) {
		bits = 2;
		p_printf(" Width out of range, set to min (%d)\n", bits);
	}

	long long old_int_mask = int_mask;

	setup_width(bits);
	mpd_del(mbits);

	p_printf(" Integers are now %d bits wide.\n", int_width);
	if (floating_mode(mode)) {
		p_printf(" In floating mode, integer width"
				" is recorded but ignored.\n");
	} else {
		// mask_stack();
		struct num *s;
		for (s = stack; s; s = s->next) {
			mpd_to_integer(s->mpd, s->mpd);
			uint64_t u = mpd_get_64_bits(s->mpd);
			/* clear any old sign extension */
			u &= (ull_t)old_int_mask;
			/* set new sign extension based on the new sign bit */
			if (u & (ull_t)int_sign_bit) {
				u |= ~(ull_t)int_mask;
			}
			mpd_set_i64(s->mpd, (int64_t)u, ctx);

		}
	}

	return GOODOP;
}


opreturn
zerof(void)
{
	return toggler(&zerofill, "Zero fill of hex/octal/binary output is now",
		"on", "off");
}

opreturn
rightalign(void)
{
	return toggler(&rightalignment, "Right alignment of integer modes is now",
		"on", "off");
}

opreturn
store(void)
{
	mpd_t *x;

	if (mpeek(&x)) {
		mpd_free_before_copy(&offstack, x, ctx);
		return GOODOP;
	}
	return BADOP;
}

opreturn
recall(void)
{
	mpush_copy(offstack);
	return GOODOP;
}

opreturn
push_pi(void)
{
	mpush_copy(pi);
	return GOODOP;
}

opreturn
push_e(void)
{
	mpush_copy(e);
	return GOODOP;
}

opreturn
mark(void)
{
	mpd_t *m;
	int64_t n;
	if (!mpop(&m))
		return BADOP;

	n = mpd_get_i64(m, ctx);
	mpd_del(m);

	if (n > stack_count || n < -1) {
		if (stack_count == 0)
			error(" error: bad mark (%d), max of 0 with empty stack, or, -1 to clear\n", (int)n);
		else
			error(" error: bad mark (%d), range between 0 and stack length (%d), or -1 to clear\n", (int)n, stack_count);
		return BADOP;
	}

	if (n == -1)
		stack_mark = 0; // special case:  clear the mark
	else
		stack_mark = stack_count - (int)n;
	return GOODOP;
}

opreturn
clearsnapshot(void)
{
	// clear existing snapstack
	struct num *p;
	while ((p = snapstack)) {
		snapstack = p->next;
		mpd_del(p->mpd);
		free(p);
	}
	return GOODOP;
}
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

	clearsnapshot();

	// copy (as much as we want of the) real stack to snapstack
	p = stack;
	snapstack = NULL;
	int i = 0;
	int n = stack_count;
	while (n > stack_mark) {
		struct num *np;

		// push a new copy of the entry on snapstack
		np = (struct num *)safe_calloc(sizeof(struct num));
		np->mpd = mpd_new(ctx);
		mpd_copy(np->mpd, p->mpd, ctx);
		np->next = snapstack;
		snapstack = np;

		// next item from "real" stack
		p = p->next;
		n--;
		i++;
	}
	p_printf(" Made snapshot of %d stack entries\n", i);

	return GOODOP;
}

opreturn
restore(void)
{
	struct num *p = snapstack;
	int i = 0;

	stack_mark = stack_count;

	while (p) {
		mpush(p->mpd);
		p = p->next;
		i++;
	}
	p_printf(" Restored %d stack entries\n", i);
	return GOODOP;
}

opreturn
sum_worker(boolean do_sum)
{

	if (stack_count <= stack_mark) {
		error(" error: empty stack, or at mark?\n");
		return BADOP;
	}

	// save a  snapshot, but don't overwrite existing
	if (!snapstack)
		snapshot();

	mpd_t *a, *n;
	mpd_t *tot, *tot_sq;
	n = mpd_new(ctx);
	tot = mpd_new(ctx);
	mpd_set_i64(tot, 0, ctx);
	tot_sq = mpd_new(ctx);
	mpd_set_i64(tot_sq, 0, ctx);
	int i = 0;
	while (stack_count > stack_mark) {
		if (!mpop(&a)) {
			fprintf(stderr, "bailing in sum_worker\n");
			goto cleanup;
		}
		// tot += a
		mpd_add(tot, tot, a, ctx);
		// tot_sq += (a * a)
		mpd_mul(a, a, a, ctx);
		mpd_add(tot_sq, tot_sq, a, ctx);
		mpd_del(a);
		i++;
	}
	mpd_set_i64(n, i, ctx);

	switch (do_sum) {
	case 1: // sum
		mpush_copy(tot);
		p_printf(" Summed %d stack entries\n", i);
		break ;;
	case 2: // average
		mpd_div(n, tot, n, ctx);
		mpush_copy(n);
		p_printf(" Averaged %d stack entries\n", i);
		break ;;
	case 3: // standard deviation
		// "oh look!  let's borrow the user's calculator!"
		// sqrt( ( (n * tot_sq) - (tot * tot)) / (n * (n-1)) )
		// in rpn:  n tot_sq * tot tot * - n n 1 - * / sqrt
#define p_(n) mpush_copy(n)
		p_(n); p_(tot_sq); multiply();
		p_(tot); p_(tot); multiply();
		subtract();
		p_(n);
		p_(n); p_(one); subtract();
		multiply();
		divide();
		squarert();
		p_printf(" Sample standard deviation calculated "
					"for %d stack entries.\n", i);
		if (i > 1) {
			p_printf(" (multiply by sqrt(%d/%d) for "
				"population standard deviation)\n", i - 1, i);
		}
		break ;;
	}
    cleanup:
	mpd_del(n);
	mpd_del(tot);
	mpd_del(tot_sq);

	return GOODOP;
}

opreturn
sum(void)
{
	return sum_worker(1);
}

opreturn
avg(void)
{
	return sum_worker(2);
}

opreturn
stddev(void)
{
	return sum_worker(3);
}

opreturn
unit_worker( int muldiv, char *factor, char *offset)
{

#define MUL '*'
#define DIV '/'

	mpd_t *a;
	static mpd_t *f, *o;

	if (!f) {
		f = mpd_new(ctx);
		o = mpd_new(ctx);
	}


	if (!mpop(&a))
		return BADOP;

	set_lastx(a);

	mpd_set_string(f, factor, ctx);
	if (offset)
		mpd_set_string(o, offset, ctx);


	switch (muldiv) {
	case MUL:
		mpd_mul(a, a, f, ctx);
		if (offset)
			mpd_add(a, a, o, ctx);
		break;
	case DIV:
		if (offset)
			mpd_sub(a, a, o, ctx);
		mpd_div(a, a, f, ctx);
		break;
	}
	mpush(a);

	return GOODOP;
}

opreturn
units_in_mm(void)
{
	return unit_worker(MUL, "25.4", 0);
}

opreturn
units_mm_in(void)
{
	return unit_worker(DIV, "25.4", 0);
}

opreturn
units_ft_m(void)
{
	return unit_worker(DIV, "3.28084", 0);
}

opreturn
units_m_ft(void)
{
	return unit_worker(MUL, "3.28084", 0);
}

opreturn
units_F_C(void)
{
	return unit_worker(DIV, "1.8", "32.0");
}

opreturn
units_C_F(void)
{
	return unit_worker(MUL, "1.8", "32.0");
}

opreturn
units_l_qt(void)
{
	return unit_worker(MUL, "1.05669", 0);
}

opreturn
units_qt_l(void)
{
	return unit_worker(DIV, "1.05669", 0);
}

opreturn
units_oz_g(void)
{
	return unit_worker(MUL, "28.3495", 0);
}

opreturn
units_g_oz(void)
{
	return unit_worker(DIV, "28.3495", 0);
}

opreturn
units_oz_ml(void)
{
	return unit_worker(MUL, "29.5735", 0);
}

opreturn
units_ml_oz(void)
{
	return unit_worker(DIV, "29.5735", 0);
}

opreturn
units_mi_km(void)
{
	return unit_worker(MUL, "1.609344", 0);
}

opreturn
units_km_mi(void)
{
	return unit_worker(DIV, "1.609344", 0);
}

opreturn
units_deg_rad(void)
{
	mpd_t *a;

	if (!mpop(&a))
		return BADOP;

	set_lastx(a);
	mpd_degrees_to_radians(a, a);
	mpush(a);

	return GOODOP;
}

opreturn
units_rad_deg(void)
{
	mpd_t *a;

	if (!mpop(&a))
		return BADOP;

	set_lastx(a);
	mpd_radians_to_degrees(a, a);
	mpush(a);

	return GOODOP;
}


opreturn
units_mpg_l100km(void)
{
	/* the same formula converts back and
	 * forth between mpg and liters/100km */
	mpd_t *t;
	int r;
	r = unit_worker(DIV, "235.214583", 0);
	if (r != GOODOP)
		return r;
	// could just call recip(), but that will change lastx
	mpop(&t);
	mpd_div(t, one, t, ctx);
	mpush(t);
	return GOODOP;
}

/* This converts -74.0444 degrees, to 74°2′40″W (expressed as "-74.0240").
 * Use "4 digits fixed" to display properly. */
opreturn
units_dd_dms(void)
{
	mpd_t *m;
	ldouble dd;
	if (!mpop(&m))
		return BADOP;

	mpd_to_double(&dd, m);

	ldouble deg, min, minsec, sec, sign;

	sign = (dd < 0) ? -1 : 1;
	dd = fabsl(dd);
	deg = floorl(dd);
	minsec = (dd - deg) * 60.0;
	min = floorl(minsec);
	sec = (minsec - min) * 60.0;
	dd = deg + min/100.0 + sec/10000.0;
	dd *= sign;

	set_lastx(m);

	mpd_from_double(m, dd, ctx);

	mpush(m);

	return GOODOP;
}

/* This converts 40°41′21.3″N, expressed as "40.41213", to 40.6892 degrees */
opreturn
units_dms_dd(void)
{
	mpd_t *m;
	ldouble dd;
	if (!mpop(&m))
		return BADOP;

	mpd_to_double(&dd, m);

	/* The variable names, identical to the routine
	 * above, don't make sense here.  But since the
	 * algorithm is identical, except for the constants,
	 * I haven't bothered changing them.
	 */
	ldouble deg, min, minsec, sec, sign;
	sign = (dd < 0) ? -1 : 1;
	dd = fabsl(dd);
	deg = floorl(dd);
	minsec = (dd - deg) * 100.0;
	min = floorl(minsec);
	sec = (minsec - min) * 100.0;
	dd = deg + min/60.0 + sec/3600.0;
	dd *= sign;

	set_lastx(m);

	mpd_from_double(m, dd, ctx);

	mpush(m);

	return GOODOP;
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

token *
talloc(void)
{
	return (struct token *)safe_calloc(sizeof(struct token));
}

void
tpush(token **tstackp, token *tok)
{
	struct token *t;

	// trace(EXEC, "tpushing on %s\n", stackname(tstackp));

	/* we may be asked to push a static or local token.  so if we
	 * originally malloc'ed the incoming token, just reuse it,
	 * otherwise malloc and copy.  */
	if (tok->alloced) {
		t = tok;
	} else {
		t = (struct token *)safe_calloc(sizeof(struct token));
		*t = *tok;
		if (tok->valstr)
			t->valstr = strdup(tok->valstr);
		if (tok->mpd) {
			t->mpd = mpd_new(ctx);
			mpd_copy(t->mpd, tok->mpd, ctx);
		}
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
tfree(token *t)
{
	if (!t) return;

	if (t->valstr) {
		free(t->valstr);
		t->valstr = 0;
	}
	if (t->mpd) {
		mpd_del(t->mpd);
		t->mpd = 0;
	}

	if (!t->alloced) {
		return;
	}

	free(t);
}

void
tclear(token **tstackp)
{
	token *t, *nt;

	t = *tstackp;
	*tstackp = NULL;

	while (t) {
		nt = t->next;
		tfree(t);
		valgrind("tclear");
		t = nt;
	}
}

void
sprint_token(char *s, size_t slen, token *t)
{
	if (!t) {
	    abort();
		snprintf(s, slen, "'null token ptr'");
		return;
	}

	switch (t->type) {
	case NUMERIC:
		// snprintf(s, slen, "'%s'", mpd_to_sci(t->mpd, 0));
		snprintf(s, slen, "'%s'", t->valstr);
		break;
	case SYMBOLIC:
		snprintf(s, slen, "'%s'", t->oper->name);
		break;
	case VARIABLE:
		snprintf(s, slen, "'%s'", t->valstr);
		break;
	case OP:
		snprintf(s, slen, "'%s'", t->oper->name);
		break;
	case EOL:
		snprintf(s, slen, "'EOL'");
		break;
	case UNKNOWN:
		snprintf(s, slen, "'UNKNOWN'");
		break;
	default:
		error(" BUG: hit default with %d in sprint_token()\n", t->type);
	}
}

void
trace_show_tok(int lev, token *t)
{
	if ((tracing & lev) == 0)
		return;

	static char buf[128];

	sprint_token(buf, sizeof(buf), t);
	fprintf(stderr, " %s ", buf);
}

void
trace_stack_dump(int lev, token **tstackp)
{
	if ((tracing & lev) == 0)
		return;

	token *t = *tstackp;

	fprintf(stderr, " %s: ", stackname(tstackp));
	if (!t)
		fprintf(stderr, "<empty>");
	else
		while (t) {
			trace_show_tok(0xff, t);
			t = t->next;
		}
	fprintf(stderr, "\n");
}

token open_paren_token, chsign_token;

void
create_infix_support_tokens()
{
	/* we'll need a couple of standalone pre-parsed tokens later
	 * on, specifically for dealing with infix processing.  */

	parse_token("(", &open_paren_token, NULL, INFIX);

	parse_token("chs", &chsign_token, NULL, INFIX);
}

void
expression_error(token *pt, token *t)
{
	char pts[128], ts[128];
	int i = 0;
	if (!pt || pt->type == UNKNOWN) {
		strcpy(pts, "start");
		i++;
	} else {
		sprint_token(pts, 128, pt);
	}
	if (!t || t->type == EOL) {
		strcpy(ts, "end");
		i++;
	} else {
		sprint_token(ts, 128, t);
	}
	if (i == 2)
		return;
	error(" error: bad expression sequence, at %s and %s\n", pts, ts);
	valgrind("expr. error");
}

opreturn
close_paren(void)
{
	/* this has to be a warning -- the command in error is already
	 * finished, so we can't cancel it. */
	error(" warning: extra parentheses\n");
	return BADOP;
}

opreturn
semicolon(void)
{
	/* In infix: (x; y) discards x
	 * but does save it as lastx.
	 * In RPN (perhaps less useful):
	 *      y x ;  discards x, just as pop would
	 */
	mpd_t *x;
	mpop(&x);
	mpd_free_before_copy(&lastx, x, ctx);
	mpd_free_before_copy(&frozen_lastx, x, ctx);
	mpd_del(x);
	return GOODOP;
}

boolean
prev_tok_was_operand(token *pt)
{
	return pt->type == NUMERIC ||
		pt->type == SYMBOLIC ||
		pt->type == VARIABLE ||
		(pt->type == OP && pt->oper->func == close_paren);
}

boolean
prev_tok_was_semicolon(token *pt)
{
	return (pt->type == OP && pt->oper->func == semicolon);
}

#define t_op t->oper	// shorthands.  don't use unless type == OP
#define pt_op pt->oper
#define tp_op tp->oper

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
 * an AI bot. ;-) */
opreturn
shunting_yard(int command)
{
	static token sytok, prevtok;
	token *t = &sytok;	// permanent pointers to sytok and prevtok
	token *pt = &prevtok;
	token *tp; // used for tpeek()
	opreturn open_paren(void);

	int nesting;

	/* by cleaning these up on the way in, we don't have to
	 * clean them up on the way out.  there are a lot more exit
	 * points than entry points. */
	tclear(&out_stack);
	tclear(&oper_stack);
	tfree(t);
	tfree(pt);

	trace(TOK,("\n infix tokens: "));

	if (command) {
		*pt = open_paren_token;
		nesting = 1;
	} else {
		pt->type = UNKNOWN;
		nesting = 0;
	}

	while (1) {
		trace(SHUNT,("\n"));
		trace_stack_dump(SHUNT,&oper_stack);
		trace_stack_dump(SHUNT,&out_stack);

		// remember, t always points at sytok
		if (!read_token(&sytok, INFIX) || sytok.type == EOL) {
			if (!infix_mode)
				break;
			sytok.type = EOL;
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

		//  ':' is bound to rpnswitch()
		if (t->type == OP && t_op->func == rpnswitch) {
			if (command || nesting) {
				error(" error: '%s' unavailable in parenthesized expression\n",
					t_op->name);
				input_ptr = NULL;
				return BADOP;
			} else if (prev_tok_was_operand(pt)) {
				putback_token(t);
				break;
			}
		}

		switch (t->type) {
		case EOL:
			if (prev_tok_was_semicolon(pt)) {
				// ';' is a no-op at EOL
				free(tpop(&out_stack));
			} else if (!prev_tok_was_operand(pt)) {
				expression_error(pt, t);
				input_ptr = NULL;
				return BADOP;
			}
			break;

		case VARIABLE:
			if (prev_tok_was_operand(pt)) {
				expression_error(pt, t);
				input_ptr = NULL;
				return BADOP;
			}
			/* do nothing now.  we need to know what comes
			 * next:  "r1 = 3" is very different than "r1 + 3" */
			trace(SHUNT, " delaying classification of %s\n",
					t->valstr);
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
				nesting++;

				if (prev_tok_was_operand(pt)) {
					expression_error(pt, t);
					input_ptr = NULL;
					return BADOP;
				}
				// Push opening parenthesis to operator stack
				tpush(&oper_stack, t);
			} else if (t_op->func == close_paren) {
				nesting--;

				if (prev_tok_was_semicolon(pt)) {
					// ';' is a no-op if followed by ')'
					free(tpop(&out_stack));
				} else if (!prev_tok_was_operand(pt)) {
					expression_error(pt, t);
					input_ptr = NULL;
					return BADOP;
				}

				// Process until matching opening paren
				while ((tp = tpeek(&oper_stack))) {
					if (tp->type == OP &&
						tp_op->func == open_paren) {
						break;
					}

					tpush(&out_stack, tpop(&oper_stack));
				}

				// Pop the opening parenthesis
				free(tpop(&oper_stack));

				/* if the parenthesized expression was
				 * an operand for a unary operator
				 * (i.e., a function), pop that too. */
				tp = tpeek(&oper_stack);
				if (tp && tp_op->operands == 1)
					tpush(&out_stack, tpop(&oper_stack));

			} else if (t_op->func == semicolon) {
				if ( pt->type == UNKNOWN ||
				    (pt->type == OP &&
					pt_op->func == open_paren)) {
					// ';' is no-op if it follows '(' or BOL
					break;
				} else if (!prev_tok_was_operand(pt)) {
					expression_error(pt, t);
					input_ptr = NULL;
					return BADOP;
				}

				// Process until matching opening paren
				while ((tp = tpeek(&oper_stack))) {
					/* might be a VARIABLE */
					if (tp->type == OP &&
						tp_op->func == open_paren) {
						break;
					}

					tpush(&out_stack, tpop(&oper_stack));
				}
				tpush(&out_stack, t);

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
						sytok = chsign_token;
						trace(TOK, " subtract is now chs\n");
					} else {  // add
						trace(TOK, " ignoring unary plus\n");
						continue;
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
			input_ptr = NULL;
			return BADOP;
		}

		// in infix mode, we're done when the line ends
		if (!command && t->type == EOL) {
			break;
		}

		// otherwise, we're done when the parentheses match
		if (command && nesting == 0) {
			break;
		}

		tfree(pt);  // safe, because "alloced" flag isn't set
		*pt = *t;

	}

	trace(SHUNT, "\n loop done:\n");
	trace_stack_dump(SHUNT,&oper_stack);
	trace_stack_dump(SHUNT,&out_stack);

	// the last step is to move the remainder of the operator stack
	while ((tp = tpeek(&oper_stack))) {
		tpush(&out_stack, tpop(&oper_stack));
	}

	if (nesting != 0) {
		error(" error: %s parentheses\n",
			nesting < 0 ? "extra" : "missing");
		input_ptr = NULL;
		return BADOP;
	}

	/* if we're in infix mode and we used our entire line of
	 * input, then trick the rpn execution into reporting that
	 * (i.e., the EOL) for us when it's finished running.  this
	 * makes autoprint work.  */
	if (infix_mode && t->type == EOL)
		tpush(&infix_rpn_queue, t);

	/* the shunting yard is done.  Dijkstra specified an
	 * output queue, but we used a stack, so it's in the wrong
	 * order.  we do one more transfer to reverse it.  the loop in
	 * main() will pull from this copy before using further user
	 * input.  */
	while((t = tpop(&out_stack)) != NULL) {
		tpush(&infix_rpn_queue, t);
	}

	trace(SHUNT, "\n merged and reversed:\n");
	trace(TOK|SHUNT, "\n");
	trace_stack_dump(TOK|SHUNT, &infix_rpn_queue);

	return GOODOP;

#undef t_op
#undef tp_op
}


opreturn
open_paren(void)
{
	return shunting_yard(1);
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
	mpd_t *m;
	uint64_t u;

	if (!mpop(&m))
		return BADOP;

	u = mpd_get_u64(m, ctx);
	mpd_del(m);

	// tracing is a bitmap of desired "feature" trace
	tracing = (int)u;

	p_printf(" internal tracing now set to %d", tracing);
	for (int i = 0; tracenames[i]; i++) {
		if (tracing & (1 << i))
			p_printf("  %s(%d)", tracenames[i], (1 << i));
	}
	p_printf("\n");

	return GOODOP;
}

void
exitret(void)
{
	if (!stack)
		exit(2);  // exit 2 on empty stack

	mpd_t *m;
	uint64_t u;

	mpop(&m);

	u = mpd_get_u64(m, ctx);
	mpd_del(m);

	exit(u == 0);  // flip exit status, per unix convention

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
    mpd_t *mpd;
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
clearvars(void)
{
	dynvar *v;

	for (v = variables; v->name; v++) {
		mpd_del(v->mpd);
		v->mpd = 0;
		free(v->name);
		v->name = 0;
	}
	return GOODOP;
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

	qsort(variables, (size_t)(v - variables), sizeof(*v), comparevars);

	int savealign = rightalignment;
	rightalignment = 0;
	for (v = variables; v->name; v++) {
		p_printf(" %20s ", v->name);
		print_n(v->mpd, mode, 0, 0);
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
		v->mpd = mpd_new(ctx);
		mpd_set_i64(v->mpd, 0, ctx);
		return v;
	}
	return 0;
}

int
dynamic_var(token *t)
{
	dynvar *v;

	v = findvar(t->valstr);
	if (!v) {
		error(" error: out of space for variables\n");
		return 0;
	}

	/* if we were preceded by '=', set our value */
	if (variable_write_enable) {
		mpd_t *x;
		if (!mpeek(&x)) {
			trace(EXEC, " nothing to assign\n");
			return 0;
		}
		// trace(EXEC, " assigning %Lg to %s\n", a, v->name);
		if (!v->mpd)
			v->mpd  = mpd_new(ctx);
		mpd_copy(v->mpd, x, ctx);
	} else {
		mpush_copy(v->mpd);
	}
	return 1;
}

size_t stralnum(char *s, char **endptr)
{
	char *ns = s;
	while (isalnum(*ns) || *ns == '_')
		ns++;
	*endptr = ns;
	return (size_t)(ns - s);
}

/* parse_token() figures out what's in the text pointed to by p., and
 * returns what it finds, in the return token t.  nextp, if non-null, is
 * set to where processing should continue */
int
parse_token(char *p, token *t, char **nextp, int whichparse)
{
	int sign = 1;
	size_t n;
	char *np;

	/* In RPN, +/- must be bound closely to numbers.  We want "1 2 -3"
	 * to push "1", "2", and "-3", not "(1-2)", and "3".
	 *
	 * But for infix expressions, how to bind +/- depends on what came
	 * before.  "(2-3)" should be read as "(2 - 3)", but "(recip-3)"
	 * should be "(recip -3)".  Here we return +/- as a separate
	 * binary operator, and it may be converted to unary in
	 * open_paren(), where we can track ordering.
	 */

	if (whichparse == RPN && (*p == '+' || *p == '-')) {
		if (match_dp(p + 1) || isdigit(*(p + 1))) { // a number?
			if (*p == '-')
				sign = -1;
			p++;
		} else if (isspace(*(p+1)) || *(p+1) == 0) { // standalone?
			goto is_oper;
		} else {
			goto unknown;
		}
	}

	if (*p == '0' && (*(p + 1) == 'x' || *(p + 1) == 'X')) {
		// hex, leading "0x"
		unsigned long long u = strtoull(p, &np, 16);

		/* be strict about what comes next */
		if (isalnum(*np))
			goto unknown;

		t->type = NUMERIC;
		t->imode = 'H';
		t->valstr = strndup(p,(size_t)(np - p));
		t->mpd = mpd_new(ctx);
		mpd_set_i64(t->mpd, (int64_t)((ll_t)u * sign), ctx);

	} else if (*p == '0' && (*(p + 1) == 'b' || *(p + 1) == 'B')) {
		// binary, leading "0b"
		p += 2;
		unsigned long long u = strtoull(p, &np, 2);

		/* be strict about what comes next */
		if (np == p || isalnum(*np))
			goto unknown;

		t->type = NUMERIC;
		t->imode = 'B';
		t->valstr = strndup(p,(size_t)(np -p));
		t->mpd = mpd_new(ctx);
		mpd_set_i64(t->mpd, (int64_t)((ll_t)u * sign), ctx);

	} else if (*p == '0' && (*(p + 1) == 'o' || *(p + 1) == 'O')) {
		// octal, leading "0o"
		p += 2;
		unsigned long long u = strtoull(p, &np, 8);

		/* be strict about what comes next */
		if (np == p || isalnum(*np))
			goto unknown;

		t->type = NUMERIC;
		t->imode = 'O';
		t->valstr = strndup(p,(size_t)(np -p));
		t->mpd = mpd_new(ctx);
		mpd_set_i64(t->mpd, (int64_t)((ll_t)u * sign), ctx);

	} else if (isdigit(*p) || match_dp(p)) {
		// decimal

		/* parse the decimal, to find its end.  we don't want
		 * the double, we just need the string */
		(void)strtold(p, &np);

		/* don't be strict about what comes next.  mistakes are
		 * less likely when entering decimal. this makes 3digits
		 *  or 18bits legal */
		if (p == np)
			goto unknown;

		if (sign < 0)
			p--;    // cover your eyes.  really.

		t->type = NUMERIC;
		t->imode = 'D';
		t->valstr = strndup(p,(size_t)(np -p));
		t->mpd = mpd_new(ctx);
		mpd_set_string(t->mpd, t->valstr, ctx);

	} else if (*p == '_' && isalnum(*(p+1))) {
		// variable
		n = stralnum(p, &np);
		t->type = VARIABLE;
		t->valstr = strndup(p,n);

	} else {

	    is_oper:
		if (isalpha(*p)) {
			n = stralnum(p, &np);
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
			np = p + n;
		} else {
			error(" error: illegal character in input\n");
			t->str = p;
			t->type = UNKNOWN;
			if (nextp) *nextp = np;
			return 0;
		}

		// command
		oper *op;

		op = opers;
		while (op->name) {
			size_t matchlen;

			if (!op->func) {
				op++;
				continue;
			}
			if (!debug_enabled && op->assoc == 'D') {
				op++;
				continue;
			}
			matchlen = strlen(op->name);
			if (n == matchlen && !strncmp(op->name, p, matchlen)) {
				np = p + matchlen;
				t->oper = op;
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
			t->str = p;
			t->type = UNKNOWN;
			if (nextp) *nextp = np;
			return 0;
		}
	}
	trace_show_tok(TOK, t);
	if (nextp) *nextp = np;
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
	if (thousands_sep[0])
		strremoveall(cp, thousands_sep);

	/* Same for currency symbols.  They're mostly unicode
	 * sequences or "$", which are safe to remove.  But some are
	 * plain ascii, or punctuation we need.  We checked earlier to
	 * be sure the currency symbol doesn't match in any of our
	 * commands.  */
	if (currency[0])
		strremoveall(cp, currency);
}

#if defined(USE_EDITLINE) || defined(USE_READLINE)
/* This supports command completion */
char *
command_generator(const char *prefix, int state)
{
	static size_t len;
	static struct oper *op;

	/* If this is the first time called, initialize our state. */
	if (!state) {
		op = opers;
		len = strlen(prefix);
	}

	/* Return the next name in the list that matches our prefix. */
	while (1) {
		if (!op->name)		// end of list
			break;
		if (!op->func || strncmp(op->name, prefix, len) != 0) {
			op++;
			continue;
		}
		return strdup((op++)->name);
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

/* get an input line from the command line editor.  returns NULL if
 * EOF, or if not reading a tty.  input_buf (i.e., *ibp) is untouched
 * in that case.  */
int
editor_line(char **ibp)
{
	char *input_buf = *ibp;
	if (!isatty(0))
		return 0;

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

	*ibp = input_buf;

	return 1;
}

#else // no editline or readline

int
editor_line(char **ibp)
{
	(void)ibp;
	return 0;
}

#endif

/* on return from fetch_line(), the global input_ptr is a string
 * containing commands to be executed, wherever they may have
 * come from (i.e., command line, environment, user input) */
int
fetch_line(void)
{
	static int arg = 1;
	static char *input_buf;
	static size_t blen;
	static boolean tried_rca_init;
	char *rca_init;

	/* get commands from $RCA_INIT */
	if (!tried_rca_init) {
		tried_rca_init = TRUE;
		rca_init = getenv("RCA_INIT");
		if (rca_init) {
			blen = strlen(rca_init) + 1;
			input_buf = safe_calloc(blen);
			strcpy(input_buf, rca_init);
			no_comments(input_buf);
			input_ptr = input_buf;
			pending_suppress();
			return 1;
		}
	}

	pending_allow();

	/* get commands from the command line.  since only numbers can
	 * start with '-', we let any other use of a hyphen bring up a
	 * usage message.  (this isn't perfectly robust, but good
	 * enough) */
	if (arg < g_argc) {

		if (g_argv[1][0] == '-' && !(isdigit(g_argv[1][1])))
			usage();

		if (input_buf) free(input_buf);

		blen = 0;
		for (arg = 1; arg < g_argc; arg++)
			blen += strlen(g_argv[arg]) + 2;

		if (blen) {
			input_buf = safe_calloc(blen);

			*input_buf = '\0';
			for (arg = 1; arg < g_argc; arg++) {
				strcat(input_buf, g_argv[arg]);
				strcat(input_buf, " ");
			}

			no_comments(input_buf);
			input_ptr = input_buf;
			return 1;
		}
	}

	/* get an input line from editline or readline */
	if (!editor_line(&input_buf)) {

		/* the command line editor didn't provide a line, so
		 * either we're running without an editor, or stdin
		 * isn't a tty.  */

		if (getline(&input_buf, &blen, stdin) < 0)  // EOF
			exitret();

		if (input_buf[strlen(input_buf) - 1] == '\n')
			input_buf[strlen(input_buf) - 1] = '\0';

		/* we might want stdin mixed with the output if we're
		 * redirecting from a file or pipe.  */
		if (echo_enabled)
			puts(input_buf);
	}

	no_comments(input_buf);

	input_ptr = input_buf;

	return 1;
}


token putback;

void
putback_token(token *t)
{
	putback = *t;
	trace(EXEC,("pushing back: ")); trace_show_tok(EXEC, t);
}

/* read_token() makes sure we have input available to parse, and sets
 * things up for parse_token() to do the work.  */
int
read_token(token *t, int parsing_rpn)
{
	char *next_input_ptr;

	bzero(t, sizeof(struct token));

	if (putback.type != UNKNOWN) {
		*t = putback;
		putback.type = UNKNOWN;
		return 1;
	}

	/* either it was never set, or we used up the data pointed
	 * to by input_ptr */
	if (input_ptr == NULL) {
		if (!fetch_line())
			return 0;
	}

	while (isspace(*input_ptr))
		input_ptr++;

	if (*input_ptr == '\0') {  // out of input -- create an EOL token
		t->type = EOL;
		// trace_show_tok(TOK, t);  // trace disabled:  too chatty
		input_ptr = NULL;
		return 1;
	}

	fflush(stdin);

	if (!parse_token(input_ptr, t, &next_input_ptr, parsing_rpn)) {
		input_ptr = NULL;
		return 0;
	}

	input_ptr = next_input_ptr;
	return 1;
}


/* the opers[] and config[] tables don't initialize everything explicitly */
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"


#define c_int 0
#define c_chr 1
#define c_str 2
#define c_none -1
struct config {
	char *command;
	int format;
	int *intstate;
	char **stringstate;
	int default_intstate;
	char *default_stringstate;
} config_table[] = {
	{ "F,D,H,O,B",		c_chr, &mode },
	{ "auto,eng,fixed",	c_str, 0, &float_specifier },
	{ "digits",		c_int, &float_digits },
	{ "width",		c_int, &int_width },
	{ "separators",		c_int, &digitseparators },
	{ "rightalign",		c_int, &rightalignment },
	{ "zerofill",		c_int, &zerofill },
	{ "autoprint",		c_int, &autoprint },
	{ "degrees",		c_int, &trig_degrees },
	{ "infix",		c_int, &infix_mode },
	{ "errorexit",		c_int, &exit_on_error },
	{ "", c_none },
	{ "debug",		c_int, &debug_enabled },
	{ "tracing",		c_int, &tracing },
	{ 0 }
};

void
config_read_defaults(void)
{
	struct config *cptr = config_table;

	while (cptr->command) {
		if (cptr->format < 0) {
			cptr++;
			continue;
		}
		switch (cptr->format) {
		case c_int:
		case c_chr:
			cptr->default_intstate = *(int *)(cptr->intstate);
			break;
		case c_str:
			cptr->default_stringstate  = *cptr->stringstate;
			break;
		}
		cptr++;
	}
}

opreturn
config(void)
{
	struct config *cptr = config_table;
	int nondefault = 0;
	char *starred;
	struct memfile rp;
	int *ip;
	char *cp, *s;

	memfile_open(&rp);

	while (cptr->command) {
		if (!cptr->command[0]) {
			if (!debug_enabled) {
				break;
			} else {
				cptr++;
				continue;
			}
		}
		if (cptr->format == c_none) // Heading: only
			p_printf(" %s", cptr->command);
		else
			p_printf(" %20s   ", cptr->command);

		starred = "    ";

		switch (cptr->format) {
		case c_int:
			ip = (int *)(cptr->intstate);
			if (*ip != cptr->default_intstate) {
				starred = "  * ";
				fprintf(rp.fp, "  %d %s", *ip, cptr->command);
				nondefault++;
			}
			p_printf("%s%d", starred, *ip);
			break;
		case c_chr:
			cp = (char *)(cptr->intstate);
			if (*cp != cptr->default_intstate) {
				starred = "  * ";
				fprintf(rp.fp, "  %c", *cp);
				nondefault++;
			}
			p_printf("%s%c", starred, *cp);
			break;
		case c_str:
			s = *cptr->stringstate;
			if (strcmp(s, cptr->default_stringstate) != 0) {
				starred = "  * ";
				fprintf(rp.fp, "  %s", s);
				nondefault++;
			}
			p_printf("%s%s", starred, s);
			break;
		}
		p_printf("\n");
		cptr++;
	}

	if (nondefault) {
	    p_printf(" Starting from defaults, recreate with:\n");
	    fflush(rp.fp);
	    p_printf("  %s\n", rp.bufp);
	}
	memfile_close(&rp);

	return GOODOP;
}

/* useful for resetting width from debugger, to generate the
 * (narrower) man page copy of the precedence table. */
size_t precedence_width = 68;

opreturn
precedence(void)
{
	oper *op;
#define NUM_PRECEDENCE 34
	static int odebug;
	static int pass = 0;
	static int assoc[NUM_PRECEDENCE];
	static char *prec_ops[NUM_PRECEDENCE];
	size_t linelen[NUM_PRECEDENCE] = {0};
	char *prefix[NUM_PRECEDENCE] = {0};
	int prec, i;

	p_printf(" Precedence for operators in infix expressions, from\n"
	       "  top to bottom in order of descending precedence.\n"
	       " All operators are left-associative, except for those\n"
	       "  in rows marked 'R', which associate right to left.\n");

	if (odebug != debug_enabled) {
		bzero(assoc, sizeof(assoc));
		bzero(prec_ops, sizeof(prec_ops));
		pass = 0;
	}

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

			if (!debug_enabled && op->assoc == 'D') {
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
				prec_ops[op->prec] = (char *)safe_calloc(500);
				prefix[op->prec] = "";
				linelen[op->prec] = 12;
			}
			if (strcmp(op->name, "chs") == 0) {
				if (*prec_ops[op->prec])
					strcat(prec_ops[op->prec], " ");
				strcat(prec_ops[op->prec], "+ -");
				linelen[op->prec] += 4;
			}
			if (!assoc[op->prec])
				assoc[op->prec] = op->assoc;
			if (*prec_ops[op->prec])
				strcat(prec_ops[op->prec], " ");
			strcat(prec_ops[op->prec], op->name);
			linelen[op->prec] += strlen(op->name) + 1;
			if (linelen[op->prec] > precedence_width) {
				linelen[op->prec] = 15;
				prefix[op->prec] = "\n              ";
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

	odebug = debug_enabled;

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
				name_fmt = 11;
			else
				name_fmt = -11;

			p_printf("%*s  %2d    %2d  %c   %s\n", name_fmt,
				op->name, op->operands, op->prec,
				op->assoc ? op->assoc : ' ',
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
	p_printf("%s\n", licensetext);
	return GOODOP;
}

/* in addition to "release", at the top of the source, the Makefile
 * may pass a -DGITVERSION=...  */
#ifndef GITVERSION
# define GITVERSION ""
#endif

char *
getversion(void)
{
	char *gitversion = GITVERSION;
	char m[8], d[8], y[8];
	static char vbuf[100];

	if (strcmp(release, GITVERSION) == 0)
		gitversion = "";
	else
		gitversion = GITVERSION;

	/* so shoot me:  I detest the extra space between month and
	 * day during the first 9 days of the month */
	sscanf(__DATE__, "%s %s %s", m, d, y);
	snprintf(vbuf, sizeof(vbuf), "version %s  %s   %s %s, %s",
			release, gitversion, m, d, y);
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
  Input can include locale currency and digit grouping symbols (e.g. $12,345)\n\
  Always prefix hex (\"0x7f\") or octal (\"0o177\"), even in hex or octal mode.\n\
  Infix expressions are entered using (...), as in: (sin(30)^2 + cos(30)^2)\n\
  Below, 'x' refers to top-of-stack, 'y' refers to the next value beneath.\n\
  rca's normal exit value reflects the logical value of the top of stack.\n\
\n");

	char cbuf[1000];
	opfunc prevfunc;

	cbuf[0] = '\0';
	prevfunc = 0;

	while (op->name) {
		if (!debug_enabled && op->assoc == 'D') {
			op++;
			continue;
		}
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
					if (op->help[0])
						fprintf(fout, "%21s     %s\n",
							cbuf, op->help);
					else
						fprintf(fout, "%21s\n", cbuf);

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
	locale = setlocale(LC_NUMERIC, NULL);

	lc = localeconv();

	decimal_pt = lc->decimal_point;
	decimal_pt_len = strlen(decimal_pt);

	/* C guarantees there will always be a locale decimal point,
	 * but nothing else that we care about.  If we're using the
	 * default locale, we default the others to (somewhat
	 * US-centric, sorry) defaults.  */
	if (strcmp(locale, "C") == 0 || strcmp(locale, "POSIX") == 0) {
		thousands_sep = ",";
		grouping = "\003";
		currency= "$";
		locale_modified = ", with added defaults";
		return;
	}

	thousands_sep = lc->thousands_sep;	// digit separator
	grouping = lc->grouping;		// digit grouping

	currency = lc->currency_symbol;

	/* make sure any non-$ currency symbol string doesn't conflict
	 * with any command name, because we're going to simply delete
	 * the symbol from input lines before parsing.  A few are
	 * known to match, or be substrings of, our commands.  */
	if (strcmp(currency, "$") != 0) {
		/* first check if it's simple ascii.  if not, no worries. */
		if (isascii(*currency)) {
			/* otherwise search for it anywhere in every command */
			oper *op = opers;
			while (op->name) {
				// if (strstr(op->name, currency)) {
				if (strcmp("CHF", currency) == 0) {
					currency = ""; // nuke it
					locale_modified = ", currency removed";
					break;
				}
				op++;
			}
		}
	}

}

// *INDENT-OFF*.
struct oper opers[] = {
//       +-------------------------------- section header if no function ptr
//       |
//       |
//       V
    {"Numeric operators with two operands:"},
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
	{"<<", lshift,		"Shift y right/left by x bits (logical shift)", 2, 22 },
	{"ror", rotateright,	0, 2, 22 },
	{"rol", rotateleft,	"Rotate y right/left by x bits", 2, 22 },
	{"&", bitwise_and,	0, 2, 20 },
	{"|", bitwise_or,	0, 2, 16 },
	{"xor", bitwise_xor,	"Bitwise AND, OR, and XOR of y and x", 2, 18 },
	{"setb", setbit,	0, 2, 16 },
	{"clearb", clearbit,	"Set and clear bit x in y", 2, 20 },
	{""},		// all-null entries cause blank line in output
    {"Numeric operators with one operand:"},
	{"~", bitwise_not,	"Bitwise NOT of x (1's complement)", 1, 30, 'R' },
	{"bitc", bitcount,	"Count of '1' bits in x", 1, 30, 'R' },
	{"chs", chsign,		0, 1, 30, 'R' },
	{"negate", chsign,	"Change sign of x (2's complement)", 1, 30, 'R' },
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
	{"dd2dms", units_dd_dms, 0, 1, 30, 'R' },
	{"dms2dd", units_dms_dd,"decimal degrees / deg.mm.sss", 1, 30, 'R' },
	{"mpg2l100km", units_mpg_l100km, "mpg to l/100km and vice versa", 1, 30, 'R' },
	{""},
    {"Constants and storage:"},
	{"sto", store,		0, 0 },
	{"rcl", recall,		"Save to or push from off-stack storage", Sym },
	{"pi", push_pi,		0, Sym },
	{"e", push_e,		"Push constant pi or e", Sym },
	{"lastx", repush,	0, Sym },
	{"lx", repush,		"Push previous value of x", Sym },
	{"_<name>", nop,	"Push variable" },  // function unused
	{"=", assignment,	"Assign variable.  RPN: \"3 = _v\"   infix: \"(_v = 3)\"", 2, 6 },
	{"variables", showvars, 0 },
	{"vars", showvars, "Show the current list of variables" },
	{"clearvariables", clearvars, "Discard all variables" },
	{""},
    {"Variadics (operate on entire stack, limited by the mark if set):"},
	{"sum", sum,		0, Auto },
	{"avg", avg,		0, Auto },
	{"stddev", stddev,	"Total, mean, and standard deviation of entries", Auto },
	{"snapshot", snapshot,	"Saves copy of selected entries", Auto },
	{"mark", mark,		"Mark the stack, to limit variadics' range" },
	{""},
    {"Stack manipulation:"},
	{"clear", clear,	"Clear stack" },
	{"pop", rolldown,	"Pop (and discard) x", Auto },
	{"push", enter,		0, Auto },
	{"dup", enter,		"Push (a duplicate of) x", Auto },
	{"exch", exchange,	0, Auto },
	{"swap", exchange,	"Exchange x and y", Auto },
	{"restore", restore,	"Push a copy of the snapshot, set mark", Auto },
	{"clearsnapshot", clearsnapshot, "Discard snapshot" },
	{""},
    {"Other:"},
	{"(", open_paren,	0, 0, 32 },
	{";", semicolon,	0, 0, 32 },
	{")", close_paren,	"Infix expression grouping", 0, 32 },
	{":", rpnswitch,	"Treat rest of line as RPN. (for infix mode)"},
	{"nop", nop,		"Does nothing, but at end of line, suppresses output"},
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
	{"B", modebin,		"Switch to floating, decimal, hex, octal, binary mode" },
	{"width", width,	0, Auto },
	{"bits", width,		"Set effective word size for integer modes", Auto },
	{"zerofill", zerof,	0, Auto },
	{"zf", zerof,		"Toggle left-fill with zeros in H, O, and B modes", Auto },
	{"rightalign", rightalign, 0, Auto },
	{"ra", rightalign,	"Toggle right alignment of numbers", Auto },
	{"degrees", use_degrees, "Toggle trig functions: degrees (1) or radians (0)" },
	{"autoprint", autop,	0 },
	{"ap", autop,		"Toggle autoprinting on/off with 0/1" },
	{"separators", separators, 0, Auto },
	{"sep", separators,	"Toggle numeric separators on/off (0/1)", Auto },
	{"mode", modeinfo,	"Display current mode parameters" },
	{"infix", infixmode,	"Toggle running mainly in infix, or in rpn" },
	{""},
    {"Debug support:", 0, 0, 0, 0, 'D'}, // hidden until "1 debug"
	{"tracing", tracetoggle,"Set tracing level", 0, 0, 'D'},
	{"commands", commands,	"Show raw command table", 0, 0, 'D'},
	{"", 0, 0, 0, 0, 'D'},
    {"Housekeeping:"},
	{"?", help,		0 },
	{"help", help,		"Show this list (using $PAGER, if set)" },
	{"config", config,	"Show current configuration settings" },
	{"state", printstate,	"Show calculator state"},
	{"precedence", precedence, "List infix operator precedence" },
	{"quit", quit,		0 },
	{"q", quit,		0 },
	{"exit", quit,		"Leave the calculator" },
	{"echo", enable_echo,	"Toggle echoing input when stdin is a file or pipe" },
	{"errorexit", enable_errexit,	"Toggle exiting on error and warning" },
	{"debug", debug,	"Toggle presence of debug commands" },
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
		if (pt->oper->operands == 0)  // pseudo op
			return;
		// 1, 2, Auto (-2) continue.  Sym (-1) is parsed as SYMBOLIC
		break;

	case SYMBOLIC:
	case VARIABLE:
		break;

	case NUMERIC:
		if ((mode == 'F' || mode == 'D') && pt->imode == 'D')
			return;
		if (pt->imode == mode)
			return;
		break;

	default:
		return;
	}

	if (tracing)  // separate any debug output from "real" output
		putchar('\n');

	print_top(mode);
}

int
main(int argc, char *argv[])
{
	static token tok, prevtok;
	token *t = &tok;	// permanent pointers to tok and prevtok
	token *pt = &prevtok;

	pt->type = UNKNOWN;
	mpd_stuff();

	char *pn = strrchr(argv[0], '/');
	progname = pn ? (pn + 1) : argv[0];

	/* fetch_line() will process args as if they were input as commands */
	g_argc = argc;
	g_argv = argv;

	locale_init();

	setup_width(0);

	create_infix_support_tokens();

	config_read_defaults();

	/* we simply loop forever, either pushing operands or
	 * executing operators.  the special end-of-line token lets us
	 * do reasonable autoprinting, if the last thing on the line
	 * was an operator.
	 */
	while (1) {

		/* use up tokens created by infix processing first */
		token *tt;
		if ((tt = tpop(&infix_rpn_queue))) {
			tok = *tt;
			free(tt);
			freeze_lastx();
		} else { /* otherwise get tokens from input as usual */
			if (!read_token(&tok, RPN))
				continue;
			thaw_lastx();
			// in infix mode, check the first token on a line...
			if (infix_mode && pt && pt->type == EOL) {
				// ...to see if it's anything but ':'
				if ( ! (t->type == OP &&
						t->oper->func == rpnswitch)) {
					putback_token(t);
					shunting_yard(0);
					continue;
				}
				/* it must have been a ':' at the
				 * start of the line.  we ignore it,
				 * and now consume tokens as usual */
			}
		}

		if (t->type != EOL && t->type != OP) {
			/* don't save pending info older than one command */
			pending_clear();
		}

		switch (t->type) {
		case NUMERIC:
			// trace(EXEC,  " numeric %s\n", t->valstr);
			trace_mpd(EXEC, "numeric", t->mpd);
			mpush(t->mpd);
			if (!tracing) {
				/* we can't see var names and numbers
				 * if we don't loosen the rules while
				 * debugging */
				free(t->valstr);
				t->valstr = 0;
			} else {
				trace(0xff, "\nWarning:  expect leak from main/"
					"read_token/parse_token/strndup\n\n");
			}
			valgrind("main numeric");
			break;
		case VARIABLE:
			trace(EXEC, " variable %s\n", t->valstr);
			dynamic_var(t);
			if (!tracing) {
				/* see above */
				free(t->valstr);
				t->valstr = 0;
			}
			valgrind("main variable");
			break;
		case SYMBOLIC:
		case OP:
			trace(EXEC, " invoking %s\n", t->oper->name);
			if (t->oper->func == quit)
				pending_show();
			else
				pending_clear();
			valgrind("pre main op (or symbolic)");
			(t->oper->func) ();
			valgrind("post main op (or symbolic)");
			break;
		case EOL:
			do_autoprint(pt);
			pending_show();
			valgrind("main eol");
			break;
		default:
		case UNKNOWN:
			// I think this is unreachable
			error(" error:  unrecognized input '%s'\n", t->str);
			valgrind("unknown");
			break;
		}
		if (variable_write_enable)
			variable_write_enable--;

		*pt = *t;

	}
	exit(3);  // not reached
}
