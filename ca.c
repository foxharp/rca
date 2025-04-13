/*
 *  build with:
 *    doit:      gcc -g -Wall -Wextra -o ca -D USE_READLINE ca.c -lm -lreadline
 *    doit-norl: gcc -g -Wall -Wextra -o ca ca.c -lm
 *
 *	This program is a mediocre but practical stack-based floating
 *	point calculator.  It resembles the UNIX 'dc' command in usage,
 *	but is not as full-featured (no variables or arrays) and is not
 *	infinite precision -- all arithmetic is done as "double".
 *	Commands resembling some of the commands common to HP scientific
 *	calculators have been added as well.
 *		- Paul G. Fox, Wed Dec 29 1993
 *
 *	[Math is done in "long double", as of 2012.]
 *
 *	It's probably still mediocre, but it still works.  Over the years
 *	its gained some unit conversions, the ability to work in integer vs.
 *	float, the ability to display and accept hex and octal, and I added
 *	readline support, for good measure.
 *		- pgf, Tue Feb 13, 2024
 *
 *	The big new feature everyone is talking about these days is the
 *	addition of "infix" operations:  a one-line parenthetical expression
 *	using traditional syntax will be evaluated, and its result left on
 *	the stack.  All of the operators available to the RPN can be used,
 *	with the addition of "X", for referencing the current top of stack.
 *	So expressions like ((X << 3) ** 2) will work.  Logical operators
 *	have been added as well:  "(X <= pi * 2)" results in 0 or 1.  (That
 *	could be written "pi 2 * >" in RPN notation.)  In addition, ca will
 *	use the logical value of its last result as its exit value, so
 *	something like 'if ca "($foo <= pi * 2)"; then ...' can be used in
 *	the shell.
 *		- pgf, Thu Apr 3, 2025
 *
 *  documentation:
 *	ca help q | less
 */

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <errno.h>
#include <locale.h>

#ifdef USE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
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

/* debugging support, runtime controllable */
int tracing;

#define trace(a)  do {if (tracing) printf a ;} while(0)

typedef int boolean;

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


/* all user input is either a number or a command operator.
 * this is how operators are looked up, by name
 */
typedef struct token token;
typedef struct oper oper;

struct oper {
	char *name;
	opreturn(*func) (void);
	char *help;
	int operands;    /* used only by infix code */
	int prec;	 /* used only by infix code */
};

/* operator table */
struct oper opers[];

/* tokens are typed -- currently numbers, operators, line-ends */
struct token {
	union {
		ldouble val;
		oper *oper;
		char *str;
	} val;
	int type;
	int alloced;
	struct token *next;  /* for stacking tokens when infix processing */
};

/* values for token type */
#define UNKNOWN -1
#define NUMERIC 0
#define SYMBOLIC 1
#define OP 2
#define EOL 3

/* 4 major modes: float, decimal, hex, octal, binary.
 * all but float are integer modes.
 */
int mode = 'f';			/* 'f', 'd', 'h', 'o', 'b' */

/* if true, print the top of stack after any line that ends with an operator */
boolean autoprint = TRUE;

/* to temporarily suppress autoprint, e.g., right after printing */
boolean suppress_autoprint = FALSE;

/* informative feedback is only printed if the command generating it
 * is followed by a newline */
char pending_info[1024];

/* if true, will decorate numbers, like "1,333,444".  */
boolean punct = TRUE;

/* Floating point precision.  This may become either the total
 * displayed precision, or the number of digits after the decimal,
 * depending on float_specifier
 */
unsigned int float_digits = 6;
int float_specifier = 'g';	/* 'f' or 'g' */
char *format_string;

/* is there a pre-defined name for this? */
#define LONGLONG_BITS (sizeof(long long) * 8)

/* These all help limit the word size to anything we want.
 */
int max_int_width;
int int_width;
long long int_sign_bit;
long long int_mask;

/* the most recent top-of-stack */
ldouble lastx;

/* copy of RPN top-of-stack, for infix use */
ldouble infix_X;

/* for store/recall */
ldouble offstack;
ldouble offstack2;
ldouble offstack3;

long long
sign_extend(ldouble a)
{
	long long b = a;

	if (int_width == LONGLONG_BITS)
		return b;
	else
		return b | (0 - (b & int_sign_bit));
}

void
push(ldouble n)
{
	struct num *p = (struct num *)calloc(1, sizeof(struct num));

	if (!p) {
		perror("ca: calloc failure");
		exit(3);
	}

	if (mode == 'f') {
		p->val = n;
		trace(("pushed %Lg/0x%llx\n", n, (long long)(p->val)));
	} else {
		p->val = sign_extend((long long)n & int_mask);
		trace(("pushed masked/extended %lld/0x%llx\n",
		(long long)(p->val), (long long)(p->val)));
	}

	p->next = stack;
	stack = p;
	stack_count++;
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
		printf(" empty stack\n");
		return FALSE;
	}
	*f = p->val;
	stack = p->next;
	trace(("popped  %Lg/0x%llx \n", p->val, (long long)(p->val)));
	free(p);
	stack_count--;

	/* remove a stack mark if we've gone below it */
	if (stack_count < stack_mark)
		stack_mark = 0;

	return TRUE;
}


token *out_stack, *oper_stack, *infix_stack;

char *
stackname(token **tstackp)
{
	char *n;
	if (tstackp == &oper_stack)
		n = "oper_stack";
	else if (tstackp == &out_stack)
		n = "out_stack";
	else if (tstackp == &infix_stack)
		n = "infix_stack";
	else
		n = "unknown stack";
	return n;
}


void
tpush(token **tstackp, token *token)
{
	struct token *t;

	/* if we originally malloc'ed the incoming token, just reuse
	 * it, otherwise malloc and copy.
	 */
	if (token->alloced) {
		t = token;
	} else {
		t = (struct token *)calloc(1, sizeof(struct token));
		if (!t) {
			perror("ca: calloc failure");
			exit(3);
		}

		*t = *token;
		t->alloced = 1;
	}

	trace(("pushed token %p to %s stack\n", t,
		(*tstackp == out_stack) ? "output":"operator"));

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
		trace(("empty tstack\n"));
		return NULL;
	}

	*tstackp = (*tstackp)->next;
	trace(("popped token %p from %s stack\n", rt, stackname(tstackp)));

	return rt;
}

void
tempty(token **tstackp)
{
	token *t, *nt;

	t = *tstackp;
	*tstackp = NULL;

	while (t) {
		nt = t->next;
		free(t);
		t = nt;
	}
}

void
tdump(token **tstackp)
{
	if (!tracing) return;

	token *t = *tstackp;

	printf("%s stack: ", stackname(tstackp));
	while (t) {
		if (t->type == NUMERIC)
			printf("%Lf  ", t->val.val);
		else if (t->type == OP)
			printf("%s  ", t->val.oper->name);
		else
			printf("t->type is %d", t->type);
		t = t->next;
	}
	printf("\n");
}


opreturn
add(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			push(a + b);
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
			push(a - b);
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
			push(a * b);
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
			if (b != 0.0) {
				push(a / b);
			} else {
				push(a);
				push(b);
				printf(" would divide by zero\n");
				return BADOP;
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
			long long i, j;

			i = (long long)a;
			j = (long long)b;
			if (j != 0) {
				push(i / j);
			} else {
				push(a);
				push(b);
				printf(" would divide by zero\n");
				return BADOP;
			}
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
y_to_the_x(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			if (a >= 0 || floorl(b) == b) {
				push(powl(a, b));
			} else {
				push(a);
				push(b);
				printf(" result would be complex\n");
				return BADOP;
			}
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
rshift(void)
{
	ldouble a, b;

	if (pop(&b)) {
		if (pop(&a)) {
			long long i, j;

			i = (long long)a;
			j = (long long)b;
			if (j < 0) {
				printf(" shift by negative not allowed\n");
				push(a);
				push(b);
				return BADOP;
			}
			push((i >> j) & ~int_sign_bit);
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

			i = (long long)a;
			j = (long long)b;
			if (j < 0) {
				printf(" shift by negative not allowed\n");
				push(a);
				push(b);
				return BADOP;
			}
			push(i << j);
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

			i = (long long)a;
			j = (long long)b;
			push(i | (1LL << j));
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

			i = (long long)a;
			j = (long long)b;
			push(i & ~(1LL << j));
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
plus(void)
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
		if (a != 0.0) {
			push(1.0 / a);
			lastx = a;
			return GOODOP;
		} else {
			push(a);
			printf(" would divide by zero\n");
			return BADOP;
		}
	}
	return BADOP;
}

opreturn
squarert(void)
{
	ldouble a;

	if (pop(&a)) {
		if (a >= 0.0) {
			push(sqrtl(a));
			lastx = a;
			return GOODOP;
		} else {
			push(a);
			printf(" can't take root of negative\n");
			return BADOP;
		}
	}
	return BADOP;
}

opreturn
trig_no_sense(void)
{
	printf(" trig functions make no sense in integer mode");
	return BADOP;
}

opreturn
sine(void)
{
	ldouble a;

	if (mode != 'f')
		return trig_no_sense();

	if (pop(&a)) {
		push(sinl((a * pi) / 180.0));
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
asine(void)
{
	ldouble a;

	if (mode != 'f')
		return trig_no_sense();

	if (pop(&a)) {
		push((180.0 * asinl(a)) / pi);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
cosine(void)
{
	ldouble a;

	if (mode != 'f')
		return trig_no_sense();

	if (pop(&a)) {
		push(cosl((a * pi) / 180.0));
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
acosine(void)
{
	ldouble a;

	if (mode != 'f')
		return trig_no_sense();

	if (pop(&a)) {
		push(180.0 * acosl(a) / pi);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
tangent(void)
{
	ldouble a;

	if (mode != 'f')
		return trig_no_sense();

	if (pop(&a)) {
		// FIXME:  tan() goes infinite at +/-90
		push(tanl(a * pi / 180.0));
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
atangent(void)
{
	ldouble a;

	if (mode != 'f')
		return trig_no_sense();

	if (pop(&a)) {
		push((180.0 * atanl(a)) / pi);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
atangent2(void)
{
	ldouble a,b;

	if (mode != 'f')
		return trig_no_sense();

	if (pop(&b)) {
		if (pop(&a)) {
			push((180.0 * atan2(a,b)) / pi);
			lastx = b;
			return GOODOP;
		}
	}
	return BADOP;
}

opreturn
fraction(void)
{
	ldouble a;

	if (pop(&a)) {
		if (a > 0)
			push(a - floorl(a));
		else
			push(a - ceill(a));
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
		if (a > 0)
			push(floorl(a));
		else
			push(ceill(a));
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

	if (pop(&lastx)) {
		while (pop(&scrap));
	}
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

opreturn
repush(void)			// aka "lastx"
{
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

void
putbinarybyte(long long mask, unsigned long long n)
{
	int i;

	n &= 0xff;
	mask &= 0xff;

	for (i = 0x80; i != 0; i >>= 1) {
		if (mask & i)
			putchar(n & i ? '1' : '0');
	}
}

void
putbinary2(int width, long long mask, unsigned long long n)
{
	int i, bytes = (width + 7) / 8;

	for (i = bytes - 1; i >= 0; i--) {
		putbinarybyte(mask >> (8 * i), n >> (8 * i));
		if (punct && i >= 1)	// commas every 8 bits
			putchar(',');
	}
}

void
putbinary(long long n)
{
	if (mode == 'f')	// no masking in float mode
		putbinary2(max_int_width, ~0, (unsigned long long)n);
	else
		putbinary2(int_width, int_mask, (unsigned long long)n);
}

void
puthex(unsigned long long n)
{
	/* commas every 4 hex digits */
	if (n < 0x10000) {
		printf("%llx", n);
		return;
	}
	puthex((n / 0x10000));
	if (punct)
		putchar(',');
	printf("%04llx", n % 0x10000);
}

void
putoct(unsigned long long n)
{
	/* commas every 3 octal digits */
	if (n < 01000) {
		printf("%llo", n);
		return;
	}
	putoct(n / 01000);
	if (punct)
		putchar(',');
	printf("%03llo", n % 01000);
}

void
print_n(ldouble n, int format)
{
	long long ln;
	long long mask = int_mask;
	long long signbit;

	suppress_autoprint = TRUE;

	if (mode == 'f')	// no masking in float mode
		mask = ~0;

	switch (format) {
	case 'h':
		ln = (long long)n & mask;
		printf(" 0x");
		puthex(ln);
		putchar('\n');
		break;
	case 'o':
		ln = (long long)n & mask;
		printf(" 0");
		putoct(ln);
		putchar('\n');
		break;
	case 'b':
		ln = (long long)n & mask;
		printf(" 0b");
		putbinary(ln);
		putchar('\n');
		break;
	case 'd':
		ln = (long long)n;
		if (mode == 'f' || int_width == LONGLONG_BITS) {
			printf(punct ? " %'lld\n" : " %lld\n", ln);
		} else {
			/* shenanigans to make pos/neg numbers
			 * appear properly.
			 */
			long long t;

			signbit = 1LL << (int_width - 1);
			mask = (long long)int_mask & ~signbit;
			if (ln & signbit) {	// negative
				t = signbit - (ln & mask);
				printf(" -");
			} else {
				t = ln & mask;
				printf(" ");
			}
			printf(punct ? "%'lld\n" : "%lld\n", t);
		}
		break;
	default:		// 'f'
		printf(format_string, float_digits, n);
		break;
	}
}

void
print_top(int format)
{
	if (stack)
		print_n(stack->val, format);
}

void
printstack(struct num *s)
{
	if (!s)
		return;

	if (s->next)
		printstack(s->next);

	print_n(s->val, mode);
}

opreturn
printall(void)
{
	printstack(stack);
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
	print_top('h');
	return GOODOP;
}

opreturn
printoct(void)
{
	print_top('o');
	return GOODOP;
}

opreturn
printbin(void)
{
	print_top('b');
	return GOODOP;
}

opreturn
printdec(void)
{
	print_top('d');
	return GOODOP;
}

opreturn
printfloat(void)
{
	print_top('f');
	return GOODOP;
}

/* debug support -- hidden command */
opreturn
printraw(void)
{
	struct num *s;

	printf("int_mask 0x%llx, int_sign_bit 0x%llx\n",
			int_mask, int_sign_bit);

	printf("stack:\n");
	s = stack;
	printf("%16s   %16s\n", "(long long)", "(long double)");
	while (s) {
		printf("%#16llx   %#16Lg\n", (long long)(s->val), s->val);
		s = s->next;
	}
	printf("stack_count %d, stack_mark %d\n", stack_count, stack_mark);
	printf("native sizes (bits):\n");
	printf("%16lu   %16lu\n", (unsigned long)(8 * sizeof(long long)),
		(unsigned long)(8 * sizeof(long double)));
	printf("long double mantissa width %d\n", LDBL_MANT_DIG);

	printf("format string for float mode: \"%s\"\n", format_string);

	suppress_autoprint = TRUE;
	return GOODOP;
}

/* debug support -- hidden command */
opreturn
tracetoggle(void)
{
	tracing = !tracing;
	return GOODOP;
}

static char *
mode2name(void)
{
	switch (mode) {
	case 'f':
		return "float";
	case 'd':
		return "decimal";
	case 'o':
		return "octal";
	case 'h':
		return "hex";
	case 'b':
		return "binary";
	default:
		printf(" mode is 0x%x\n", mode);
		return "ERROR";
	}
}

void
showmode(void)
{

	printf(" Mode is %s. ", mode2name());

	if (mode == 'f') {
		char *msg;

		if (float_specifier == 'g') {
			/* float_digits == 7 gives:  123.4560  */
			msg = "of total precision";
		} else {	/* 'f' */
			/* float_digits == 7 gives:  123.4560000  */
			msg = "after the decimal";
		}
		printf(" Displaying %u digits %s.\n", float_digits, msg);
	} else {
		printf(" Integer math with %d bits.\n", int_width);
	}

	suppress_autoprint = TRUE;
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
	mode = 'h';
	showmode();
	return printall();
}

opreturn
modebin(void)
{
	mode = 'b';
	showmode();
	return printall();
}

opreturn
modeoct(void)
{
	mode = 'o';
	showmode();
	return printall();
}

opreturn
modedec(void)
{
	mode = 'd';
	showmode();
	return printall();
}

opreturn
modefloat(void)
{
	mode = 'f';
	showmode();
	return printall();
}

void
setup_format_string(void)
{
	/* The floating print options include
	   - commas or not
	   - alternate form or not (we use it with %f, but not %g)
	   - %f or %g
	   Also:
	   - precision
	   but happily that's provided via the '*' specifier at printf time.

	   So there are just four forms to deal with here.
	 */

	if (punct) {
		if (float_specifier == 'f')
			format_string = " %'#.*Lf\n";
		else
			format_string = " %'.*Lg\n";
	} else {
		if (float_specifier == 'f')
			format_string = " %#.*Lf\n";
		else
			format_string = " %.*Lg\n";
	}
}

opreturn
precision(void)
{
	ldouble digits;

	if (!pop(&digits))
		return BADOP;

	float_digits = abs((int)digits);
	if (float_digits < 1)
		float_digits = 1;

	float_specifier = 'g';

	setup_format_string();

	// info
	snprintf(pending_info, sizeof(pending_info),
		" %d digit%s of total displayed precision.\n",
		float_digits, float_digits == 1 ? "" : "s");

	return GOODOP;
}

opreturn
punctuation(void)
{
	punct = !punct;
	// info
	snprintf(pending_info, sizeof(pending_info),
		" numeric punctuation is now %s\n", punct ? "on" : "off");
	setup_format_string();
	return GOODOP;
}

opreturn
decimal_length(void)
{
	ldouble digits;

	if (!pop(&digits))
		return BADOP;

	float_digits = abs((int)digits);
	if (float_digits < 1)
		float_digits = 1;

	float_specifier = 'f';

	setup_format_string();

	// info
	snprintf(pending_info, sizeof(pending_info),
		" %d digit%s after the decimal.\n",
		float_digits, float_digits == 1 ? "" : "s");

	return GOODOP;
}

void
setup_width(int bits)
{
	/* we use long double to store our data.  in integer mode,
	 * this means the FP mantissa, if it's shorter than long long,
	 * may limit our maximum word width.
	 */
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

	if (int_width == LONGLONG_BITS)
		int_mask = ~0;
	else
		int_mask = (1LL << int_width) - 1;
}

opreturn
width(void)
{
	ldouble n;
	long long bits;

	if (!pop(&n))
		return BADOP;

	bits = n;
	if (bits > max_int_width) {
		bits = max_int_width;
		printf(" Width out of range, set to max (%lld)\n", bits);
	}
	if (bits < 8) {
		bits = 8;
		printf(" Width out of range, set to min (%lld)\n", bits);
	}

	setup_width(bits);

	// info
	snprintf(pending_info, sizeof(pending_info),
		" Words are %d bits wide.%s\n", int_width,
			(mode == 'f') ? "  (Ignored in float mode!)":"");
	// This is sort of an "info" message, except that it also
	// does a big printall() down below.  So we'll keep printing
	// it mid-commandline, for now.
	if (*pending_info)
		printf("%s", pending_info);
	*pending_info = '\0';

	/* need to mask and sign extend anything on the stack if we've
	 * shortened word length.
	 */
	struct num *s;

	for (s = stack; s; s = s->next)
		s->val = sign_extend((long long)s->val & int_mask);

	return printall();
}

opreturn
store(void)
{
	ldouble a;

	if (pop(&a)) {
		push(a);
		offstack = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
store2(void)
{
	ldouble a;

	if (pop(&a)) {
		push(a);
		offstack2 = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
store3(void)
{
	ldouble a;

	if (pop(&a)) {
		push(a);
		offstack3 = a;
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
recall2(void)
{
	push(offstack2);
	return GOODOP;
}

opreturn
recall3(void)
{
	push(offstack3);
	return GOODOP;
}

opreturn
push_pi(void)
{
	push(pi);
	return GOODOP;
}

opreturn
push_e(void)
{
	push(e);
	return GOODOP;
}

opreturn
mark(void)
{
	stack_mark = stack_count;
	return GOODOP;
}

opreturn
stack_x(void)
{
	push(infix_X);
	return GOODOP;
}

opreturn
sum(void)
{
	opreturn r;
	ldouble a, tot = 0;

	if (stack_count <= stack_mark) {
		printf(" nothing to sum\n");
		return BADOP;
	}
	while (stack_count > stack_mark) {
		if ((r = pop(&a)) == BADOP)
			break;
		tot += a;
	}
	push(tot);
	stack_mark = 0;
	return r;
}

opreturn
units_in_mm(void)
{
	ldouble a;

	if (pop(&a)) {
		a *= 25.4;
		push(a);
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
		push(a);
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
		push(a);
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
		push(a);
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
		push(a);
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
		push(a);
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
		push(a);
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
		push(a);
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
		push(a);
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
		push(a);
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
		push(a);
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
		push(a);
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
autop(void)
{
	autoprint = !autoprint;
	// info
	snprintf(pending_info, sizeof(pending_info),
		" autoprinting is now %s\n", autoprint ? "on" : "off");
	return GOODOP;
}

void
exitret(void)
{
	ldouble a = 0;
	if (stack) {
		pop(&a);
		exit(a == 0);  // flip status per unix convention
	} else {
		exit(2);
	}

}

opreturn
quit(void)
{
	if (!suppress_autoprint && autoprint)
		print_top(mode);
	exitret();
	return GOODOP; // not reached
}

size_t stralnum(char *s, char **endptr)
{
	char *ns = s;
	while (isalnum(*ns))
		ns++;
	*endptr = ns;
	return ns - s;
}

size_t strpunct(char *s, char **endptr)
{
	char *ns = s;
	while (ispunct(*ns))
		ns++;
	*endptr = ns;
	return ns - s;
}

int
parse_tok(char *p, token *t, char **nextp, int rpn_signed_ints)
{
	int sign = 1;

	/* when parsing infix, we must treat "-3" as a unary negation
	 * operator, followed by the positive number "3".  That's
	 * special-cased in open_paren().  In RPN, it must be treated
	 * as negative 3.
	 */
	if (rpn_signed_ints) {
		/* be sure + and - are bound closely to numbers */
		if (*p == '+' && (*(p + 1) == '.' || isdigit(*(p + 1)))) {
			p++;
		} else if (*p == '-' && (*(p + 1) == '.' || isdigit(*(p + 1)))) {
			sign = -1;
			p++;
		}
	}

	if (*p == '0' && (*(p + 1) == 'x' || *(p + 1) == 'X')) {
		// hex
		long long ln = strtoull(p, nextp, 16);

		if (ln == 0 && p == *nextp)
			goto unknown;
		t->type = NUMERIC;
		t->val.val = ln * sign;
		return 1;

	} else if (*p == '0' && (*(p + 1) == 'b' || *(p + 1) == 'B')) {
		// binary
		long long ln = strtoull(p + 2, nextp, 2);

		if (ln == 0 && p == *nextp)
			goto unknown;
		t->type = NUMERIC;
		t->val.val = ln * sign;
		return 1;

	} else if (*p == '0' && ('0' <= *(p + 1) && *(p + 1) <= '7')) {
		// octal
		long long ln = strtoull(p, nextp, 8);

		if (ln == 0 && p == *nextp)
			goto unknown;
		t->type = NUMERIC;
		t->val.val = ln * sign;
		return 1;

	} else if (isdigit(*p) || (*p == '.')) {
		// decimal
		long double dd = strtold(p, nextp);

		if (dd == 0.0 && p == *nextp)
			goto unknown;
		t->type = NUMERIC;
		t->val.val = dd * sign;
		return 1;
	} else {
		int n;
		if (isalpha(*p)) {
			n = stralnum(p, nextp);
		} else if (ispunct(*p)) {
			/* parser hack:  hard-coded list of
			 * double punctuation operators */
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
			printf(" illegal character in input\n");
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
				if (op->operands == -1)
				    t->type = SYMBOLIC;  // like "pi", "recall"
				else
				    t->type = OP;
				return 1;
			}
			op++;
		}
		if (!op->name) {
		unknown:
			t->val.str = p;
			t->type = UNKNOWN;
			return 0;
		}
	}
	return 1;
}

void
no_comm(char *cp)
{
	char *ncp;

	/* first eliminate comments */
	if ((ncp = strchr(cp, '#')) != NULL)
		*ncp = '\0';

	/* then eliminate commas from numbers, like "1,345,011".  this
	 * removes them from the whole line; a side-effect is that
	 * there can be no commas in commands.
	 */
	ncp = cp;
	while ((ncp = strchr(ncp, ',')) != NULL)
		memmove(ncp, ncp + 1, strlen(ncp));

	/* same for '$' signs */
	ncp = cp;
	while ((ncp = strchr(ncp, '$')) != NULL)
		memmove(ncp, ncp + 1, strlen(ncp));
}

static char *input_ptr = NULL;

void
flushinput(void)
{
	input_ptr = NULL;
}

/* on return, the global input_ptr is a string containing commands
 * to be executed
 */
int
fetch_line(void)
{
	static int arg = 1;
	static char *input_buf;
	static size_t blen;

	/* if there are args on the command line, they're taken as
	 * initial commands.  since only numbers can start with '-',
	 * any other use of a hyphen brings up a usage message.
	 */
	if (arg < g_argc) {

		if (g_argv[1][0] == '-' && !(isdigit(g_argv[1][1])))
			usage();

		blen = 0;
		for (arg = 1; arg < g_argc; arg++)
			blen += strlen(g_argv[arg]) + 2;

		input_buf = malloc(blen);
		if (!input_buf) {
			perror("ca: malloc failure");
			exit(3);
		}

		*input_buf = '\0';
		for (arg = 1; arg < g_argc; arg++) {
			strcat(input_buf, g_argv[arg]);
			strcat(input_buf, " ");
		}

		no_comm(input_buf);

		input_ptr = input_buf;
		return 1;
	}
#ifdef USE_READLINE

	static char readline_init_done = 0;

	if (!readline_init_done) {	// readline initializations

		using_history();

		/* prevent readline doing tab filename completion */
		rl_bind_key_in_map('\t', rl_insert,
			rl_get_keymap_by_name("emacs"));
		rl_bind_key_in_map('\t', rl_insert,
			rl_get_keymap_by_name("vi-insert"));

		readline_init_done = 1;
	}

	/* if we used the buffer as input, add it to history.  doing
	 * this here records any command line input, possibly stored
	 * in the buffer above, on the first call to fetch_line()
	 */
	if (input_buf && *input_buf)
		add_history(input_buf);

	if ((input_buf = readline("")) == NULL)  // got EOF
		exitret();

	// readline() doesn't echo bare newlines to tty, so do it here,
	if (*input_buf == '\0')
		putchar('\n');

#else
	if (getline(&input_buf, &blen, stdin) < 0)  // EOF
		exitret();

	/* if stdin is a terminal, the command is already on-screen.
	 * but we also want it mixed with the output if we're
	 * redirecting from a file or pipe.  (easy to get rid of it
	 * with something like: "ca < commands | grep '^ '"
	 */
	if (!isatty(0))
		printf("%s", input_buf);

#endif

	no_comm(input_buf);

	input_ptr = input_buf;

	return 1;
}

int
gettoken(struct token *t)
{
	if (input_ptr == NULL)
		if (!fetch_line())
			return 0;

	while (isspace(*input_ptr))
		input_ptr++;

	if (*input_ptr == '\0') {	/* out of input */
		t->type = EOL;
		input_ptr = NULL;
		return 1;
	}

	fflush(stdin);

	if (!parse_tok(input_ptr, t, &input_ptr, 1)) {
		printf(" error: unrecognized input '%s'\n", input_ptr);
		// flushinput();
		return 0;
	}

	return 1;
}

token open_paren_token, chsign_token, plus_token;

void
create_infix_support_tokens()
{
	/* we need a couple of token identifiers for later on,
	 * specifically for dealing with infix processing.
	 */
	char *outp;
	(void)parse_tok("(", &open_paren_token, &outp, 0);
	(void)parse_tok("chs", &chsign_token, &outp, 0);
	(void)parse_tok("plus", &plus_token, &outp, 0);
}

opreturn
close_paren(void)
{
	// this has to be a warning -- the command in error is already
	// finished, so we can't cancel it.
	printf(" warning: mismatched/extra parentheses\n");
	return BADOP;
}

/* This implementation of Dijkstra's "shunting yard" algorithm, for
 * translating an infix expression to RPN, is based roughly on the
 * pseudocode at Wikipedia, at brilliant.org, and on several of the coded
 * examples at the rosettacode.org.
 *  https://en.wikipedia.org/wiki/Shunting_yard_algorithm
 *  https://brilliant.org/wiki/shunting-yard-algorithm/
 *  https://rosettacode.org/wiki/Parsing/Shunting-yard_algorithm
 * Also (and perhaps mostly!) on pseudo-code offered up by Google's AI
 * in response to a search.  See shunting.pseudocode in source dir.
 */
opreturn
open_paren(void)
{
	static struct token tok, ptok;
	token *t, *tp;
	int operands, precedence;
	int paren_count = 1;

	ptok.type = UNKNOWN;

	tempty(&out_stack);
	tempty(&oper_stack);

	// push the '(' token that the user typed, but won't be parsed.
	tpush(&oper_stack, &open_paren_token);

	trace(("collecting infix line\n"));

	// set the source for the 'X' operator to the current top
	// of the RPN stack, or zero if that stack is empty.
	if (!peek(&infix_X))
		infix_X = 0;

	while (1) {
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

		switch (t->type) {
		case NUMERIC:
		case SYMBOLIC:
			if (t->type == NUMERIC)
			    trace(("val is %Lf\n", t->val.val));
			else
			    trace(("symbolic is %s\n", t->val.oper->name));
			if (ptok.type == NUMERIC || ptok.type == SYMBOLIC) {
				printf(" error: bad expression sequence\n");
				// flushinput();
				return BADOP;
			}
			tpush(&out_stack, t);
			break;
		case OP:
			operands = t->val.oper->operands;
			precedence = t->val.oper->prec;

			trace(("oper is %s\n", t->val.oper->name ));

			tp = tpeek(&oper_stack);

			if (t->val.oper->func == open_paren) {
				// Push opening parenthesis to operator stack
				tpush(&oper_stack, t);
				paren_count++;
			} else if (t->val.oper->func == close_paren) {
				// Process until matching opening parenthesis
				while (1) {
					if (tp == NULL) {
						printf(" error: missing parentheses?\n");
						return BADOP;
					}

					if (tp->val.oper->func == open_paren)
						break;

				    	if (ptok.type == OP &&
					    ptok.val.oper->operands > 0) {
						printf(" error: missing operand(s) for %s\n",
							tp->val.oper->name);
						return BADOP;
					}

					tpush(&out_stack, tpop(&oper_stack));
					tp = tpeek(&oper_stack);
				}

				// Pop the opening parenthesis
				free(tpop(&oper_stack));
				tp = tpeek(&oper_stack);
				if (tp && tp->val.oper->operands == 1) {
					tpush(&out_stack, tpop(&oper_stack));
				}
				paren_count--;

			} else if (operands == 1) { // one operand, like "~"
			unary:
				while (tp != NULL &&
					(tp->val.oper->func != open_paren) &&
					(tp->val.oper->prec >= precedence))
				{
					tpush(&out_stack, tpop(&oper_stack));
					tp = tpeek(&oper_stack);
				}
				tpush(&oper_stack, t);

			} else if (operands == 2) { // two operands
				// Special cases:  '-' and '+' are
				// either binary or unary.  They're
				// unary if they come first, or follow
				// another operator.  A closing paren
				// is not an operator in this case.
				if (ptok.type == UNKNOWN ||
				    		(ptok.type == OP &&
					ptok.val.oper->func != close_paren)) {
					if (t->val.oper->func == subtract) {
						t = &chsign_token;
						precedence = t->val.oper->prec;
						goto unary;
					}
					if (t->val.oper->func == add ) {
						t = &plus_token;
						precedence = t->val.oper->prec;
						goto unary;
					}
				}

				/* two two operand ops in a row? */
				if (ptok.type == OP &&
					ptok.val.oper->func != close_paren) {
					printf(" error: bad operator sequence\n");
					// flushinput();
					return BADOP;
				}

				// Handle binary operators
				while (tp != NULL) {
					if (tp->val.oper->func == open_paren)
						break;
					/* left-associative by default */
					if (tp->val.oper->prec < precedence)
						break;
					/* a ** b is right-associative */
					if (tp->val.oper->prec <= precedence &&
					    tp->val.oper->func == y_to_the_x)
						break;

					tpush(&out_stack, tpop(&oper_stack));
					tp = tpeek(&oper_stack);
				}
				tpush(&oper_stack, t);
			} else {
				printf(" error: '%s' unsuitable in infix expression\n",
					t->val.oper->name);
				// flushinput();
				return BADOP;
			}
			break;

		default:
		case UNKNOWN:
		cleanup:
			printf(" error: unrecognized input '%s'\n", t->val.str);
			// flushinput();
			return BADOP;
		}

		if (paren_count == 0)
			break;

		ptok = *t;

	}
	tdump(&oper_stack);
	tdump(&out_stack);

	if (paren_count) {
		printf(" error: missing parentheses\n");
		// flushinput();
		return BADOP;
	}

	trace(("final move to out_stack\n"));

	/* move what's on the operator stack to the output stack */
	while ((t = tpop(&oper_stack)) != NULL) {
		tpush(&out_stack, t);
	}


	fflush(stdout);

	tdump(&oper_stack);
	tdump(&out_stack);

	/* and with that, the shunting yard is finished.
	 * unfortunately, the output stack is in the wrong order.  so
	 * we do one more transfer to reverse it.  gettoken() will
	 * pull from this copy.
	 */
	trace(("moving to infix stack\n"));
	while((t = tpop(&out_stack)) != NULL) {
		tpush(&infix_stack, t);
	}
	tdump(&infix_stack);

	return GOODOP;
}


opreturn
precedence(void)
{
	int prec;
	oper *op;
	char *prec_ops[40] = {0};

	op = opers;
	while (op->name) {
		if (op->name[0] && op->func && op->prec > 0) {
			if (op->prec >= 40) {
				printf("%s precedence too large: %d\n",
				    op->name, op->prec);
			}
			if (!prec_ops[op->prec])
				prec_ops[op->prec] = (char *)calloc(1, 500);
			strcat(prec_ops[op->prec], op->name);
			strcat(prec_ops[op->prec], " ");
		}
		op++;
	}

	for (prec = 0; prec < 40; prec++) {
		if (prec_ops[prec])
			printf("%d	%s\n", prec, prec_ops[prec]);
	}

	return GOODOP;
}

opreturn
table(void)
{
	oper *op, *lastop = NULL;
	char *indent;

	op = opers;

	while (op->name) {
		if (op->func ) {
			indent="";
			if (lastop && lastop->func == op->func )
				indent="  ";
			printf("%s%s\t%d\t%d\t%s\n", indent,
				op->name, op->operands, op->prec, op->help ?: "");
		}
		lastop = op;
		op++;
	}
	return GOODOP;
}

opreturn
help(void)
{
	oper *op;

	op = opers;
	printf("\
ca -- a stack based calculator\n\
 Any arguments on the command line are used as initial calculator input.\n\
 Entering a number pushes it on the stack.\n\
 Operators replace either one or two top stack values with their result.\n\
 Most whitespace is optional between number and operators, and can consist\n\
  of spaces, tabs, or newlines.\n\
 Numbers can include commas and $ signs (e.g., '$3,577,455').\n\
 Numbers are represented internally as long double and signed long long.\n\
 Max integer width is the shorter of long long or the long double mantissa.\n\
 Always use 0xNNN/0NNN to enter hex/octal, even in hex or octal mode.\n\
 An infix expression may be started with '('.  The evaluated result\n\
  goes on the stack.  For example, '(sqrt(sin(30)^2 + cos(30)^2) + 2)' will\n\
  push the value '3'.  All operators and functions, all unit conversions, and\n\
  all commands that produce constants (e.g., 'pi', 'recall') can be referenced\n\
  in infix expressions.  The infix expression must all be entered on one line.\n\
 Below, 'x' refers to top-of-stack, 'y' refers to the next value beneath. \n\
 On exit, ca returns 0 if the top of stack is non-zero, else it returns 1,\n\
 or 2 if stack is empty, and 3 in the case of program error.\n\
\n\
");
	char cbuf[1000];
	cbuf[0] = '\0';
	while (op->name) {
		if (!*op->name) {
			putchar('\n');
		} else {
			if (op->help && !strcmp(op->help, "HideMe")) {
				/* hidden command */ ;
			} else if (!op->func) {
				printf("%s\n", op->name);
			} else if (!op->help) {
				strcat(cbuf, " ");
				strcat(cbuf, op->name);
				strcat(cbuf, ",");
			} else {
				strcat(cbuf, " ");
				strcat(cbuf, op->name);
				printf("%20s     %s\n", cbuf, op->help);
				cbuf[0] = '\0';
			}
		}
		op++;
	}
	printf("\n%78s\n", __FILE__ " built " __DATE__ " " __TIME__);
	printf ("\nTip:  Use \"ca help q | less\" to view this help\n");
	return GOODOP;
}

/* the opers[] table doesn't initialize everything explicitly */
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

// *INDENT-OFF*.
struct oper opers[] = {
//       +-------------------------------- section header if no function ptr
//       |                           +---- function pointer
//       |                           |
//       V                           V
    {"Operators with two operands:", 0, 0},
//        +------------------------------- operator names
//        |    +-------------------------- function pointer
//        |    |                +--------- help (if 0, shares next cmd's help)
//        |    |                |  +------ # of operands (-1 for none)
//        |    |                |  |  +--- operator precedence
//        |    |                |  |  |         (# of operands and precedence
//        V    V                V  V  V           used only by infix code)
	{"+", add,		0, 2, 22 },
	{"-", subtract,		"Add and subtract x and y", 2, 22 },
	{"*", multiply,		0, 2, 23 },
	{"x", multiply,		"Two ways to multiply x and y", 2, 23 },
	{"/", divide,		0, 2, 23 },
	{"%", modulo,		"Divide and modulo of y by x (arithmetic shift)", 2, 23 },
	{"^", y_to_the_x,	0, 2, 24 },
	{"**", y_to_the_x,	"Raise y to the x'th power", 2, 24 },
	{">>", rshift,		0, 2, 19 },
	{"<<", lshift,		"Right/left logical shift of y by x bits", 2, 19 },
	{"&", bitwise_and,	0, 2, 18 },
	{"|", bitwise_or,	0, 2, 16 },
	{"xor", bitwise_xor,	"Bitwise AND, OR, and XOR of y and x", 2, 17 },
	{"setb", setbit,	0, 2, 16 },
	{"clearb", clearbit,	"Set and clear bit x in y", 2, 18 },
	{"", 0, 0},		// all-null entries cause blank line in output
    {"Operators with one operand:", 0, 0},
	{"~", bitwise_not,	"Bitwise NOT of x (1's complement)", 1, 31 },
	{"chs", chsign,		0, 1, 31 },  // precedence unused, see special case in open_paren()
	{"negate", chsign,	"Change sign of x (2's complement)", 1, 31 },
	{"plus", plus,		"HideMe", 1, 31 }, // needed for infix
	{"recip", recip,        0, 1, 30 },
	{"sqrt", squarert,      "Reciprocal and square root of x", 1, 30 },
	{"sin", sine,           0, 1, 30 },
	{"cos", cosine,         0, 1, 30 },
	{"tan", tangent,        "", 1, 30 },
	{"asin", asine,         0, 1, 30 },
	{"acos", acosine,       0, 1, 30 },
	{"atan", atangent,      "Trig functions (in degrees)", 1, 30 },
	{"atan2", atangent2,    "Arctan of y/x (i.e., 2 operands, in degrees)", 2, 30 },

	{"abs", absolute,	0, 1, 30 },
	{"frac", fraction,	0, 1, 30 },
	{"int", integer,	"Absolute value, fractional and integer parts of x", 1, 30 },
	{"(", open_paren,	"Begin \"infix\" expression, ends at matching ')' or EOL", 0, 39 },
	{")", close_paren,	"HideMe" }, // needed for infix to work
	{"", 0, 0},
    {"Logical operators:", 0, 0},
	{"&&", logical_and,     0, 2, 4 },
	{"||", logical_or,      "Logical AND and OR", 2, 3 },
	{"==", is_eq,           0, 2, 8 },
	{"!=", is_neq,          0, 2, 8 },
	{"<", is_lt,            0, 2, 9 },
	{"<=", is_le,           0, 2, 9 },
	{">", is_gt,            0, 2, 9 },
	{">=", is_ge,           "Arithmetic comparisons", 2, 9 },
	{"!", logical_not,	"Logical NOT of x", 1, 31 },
	{"", 0, 0},
    {"Stack manipulation:", 0, 0},
	{"clear", clear,	"Clear stack" },
	{"pop", rolldown,	"Pop (and discard) x" },
	{"push", enter,		0 },
	{"dup", enter,		0 },
	{"enter", enter,	"Push (duplicate) x" },
	{"lastx", repush,	0 },
	{"lx", repush,		"Fetch previous x", -1 },
	{"exch", exchange,	0 },
	{"swap", exchange,	"Exchange x and y" },
	{"mark", mark,		"Mark stack for later summing" },
	{"sum", sum,		"Sum stack to \"mark\", or entire stack if no mark" },
	{"", 0, 0},
    {"Constants and storage:", 0, 0},
	{"store", store,	0 },
	{"sto", store,		0 },
	{"sto2", store2,	0 },
	{"sto3", store3,	"Save x off-stack (3 locations)" },
	{"recall", recall,	0, -1 },
	{"rcl", recall,		0, -1 },
	{"rcl2", recall2,	0, -1 },
	{"rcl3", recall3,	"Fetch x (3 locations)", -1 },
	{"X", stack_x,		"Copy of x for use in infix expressions", -1 },
	{"pi", push_pi,		"Push constant pi", -1 },
	{"e", push_e,		"Push constant e", -1 },
	{"", 0, 0},
    {"Conversions:", 0, 0},
	{"i2mm", units_in_mm,   0, 1, 30 },
	{"mm2i", units_mm_in,   "inches / millimeters", 1, 30 },
	{"ft2m", units_ft_m,	0, 1, 30},
	{"m2ft", units_m_ft,	"feet / meters", 1, 30 },
	{"mi2km", units_mi_km,  0, 1, 30 },
	{"km2mi", units_km_mi,  "miles / kilometers", 1, 30 },
	{"f2c", units_F_C,      0, 1, 30 },
	{"c2f", units_C_F,      "degrees F/C", 1, 30 },
	{"oz2g", units_oz_g,    0, 1, 30 },
	{"g2oz", units_g_oz,    "ounces / grams", 1, 30 },
	{"q2l", units_qt_l,     0, 1, 30 },
	{"l2q", units_l_qt,     "quarts / liters", 1, 30 },
	{"", 0, 0},
    {"Display:", 0, 0},
	{"P", printall,		"Print whole stack according to mode" },
	{"p", printone,		"Print x according to mode" },
	{"f", printfloat,	0 },
	{"d", printdec,		0 },
	{"o", printoct,		0 },
	{"h", printhex,		0 },
	{"b", printbin,		"Print x in float, decimal, octal, hex, binary" },
	{"autoprint", autop,	0 },
	{"a", autop,		"Toggle autoprinting on/off" },
	{"dR", printraw,	"HideMe" }, // raw stack contents
	{"dT", tracetoggle,	"HideMe" }, // toggle tracing
	{"", 0, 0},
    {"Modes:", 0, 0},
	{"F", modefloat,	"Switch to floating point mode" },
	{"D", modedec,		0 },
	{"I", modedec,		"Switch to decimal integer mode" },
	{"H", modehex,		0 },
	{"X", modehex,		"Switch to hex mode" },
	{"O", modeoct,		0 },
	{"B", modebin,		"Switch to octal or binary modes" },
	{"precision",		precision, 0 },
	{"k", precision,	"Float format: number of significant digits (%g)" },
	{"decimals",		decimal_length, 0 },
	{"K", decimal_length,	"Float format: digits after decimal (%f)" },
	{"width", width,	0 },
	{"w", width,		"Set effective \"word size\" for integer modes" },
	{"commas", punctuation,	0 },
	{"c", punctuation,	"Toggle numeric comma separators on/off" },
	{"mode", modeinfo,	"Display current mode parameters" },
	{"", 0, 0},
    {"Housekeeping:", 0, 0},
	{"?", help,		0 },
	{"help", help,		"Show this list" },
	{"dH", table,		"HideMe" },  // raw command table
	{"dP", precedence,	"HideMe" },  // precedence list
	{"quit", quit,		0 },
	{"q", quit,		0 },
	{"exit", quit,		"Leave the calculator" },
	{"#", help,		"Comment. The rest of the line will be ignored." },
	{NULL, NULL, 0},
};
// *INDENT-ON*.

int
main(int argc, char *argv[])
{
	struct token tok;
	token *t;
	static int lasttoktype;
	char *pn;

	pn = strrchr(argv[0], '/');
	progname = pn ? (pn + 1) : argv[0];

	/* fetch_line() will process args as if they were input as commands */
	g_argc = argc;
	g_argv = argv;

	/* apparently needed to make the %'Ld format for commas work */
	setlocale(LC_ALL, "");

	setup_width(0);
	setup_format_string();

	create_infix_support_tokens();

	/* we simply loop forever, either pushing operands or
	 * executing operators.  the special end-of-line token lets us
	 * do reasonable autoprinting, if the last thing on the line
	 * was an operator.
	 */
	while (1) {

		// use tokens created by infix processing first */
		t = tpop(&infix_stack);
		if (t) {
			tok = *t;
			free(t);
		} else {
			if (!gettoken(&tok))
				continue;
		}
		t = &tok;

		if (t->type != EOL)
			*pending_info = '\0';

		switch (t->type) {
		case NUMERIC:
			push(t->val.val);
			break;
		case SYMBOLIC:
		case OP:
			trace(( "invoking %s\n", t->val.oper->name));
			(void)(t->val.oper->func) ();
			break;
		case EOL:
			if (*pending_info)
				printf("%s", pending_info);
			*pending_info = '\0';
			if (!suppress_autoprint && autoprint &&
				(lasttoktype == OP || lasttoktype == SYMBOLIC)) {
				print_top(mode);
			}
			suppress_autoprint = FALSE;
			break;
		default:
		case UNKNOWN:
			printf(" error: unrecognized input '%s'\n", t->val.str);
			// flushinput();
			break;
		}

		lasttoktype = t->type;

	}
	exit(3);  // not reached
}
