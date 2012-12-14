
/* 
 *	This program is a mediocre but practical stack-based floating
 *	point calculator.  It resembles the UNIX 'dc' command in usage,
 *	but is not as full-featured (no variables or arrays) and is not
 *	infinite precision -- all arithmetic is done as "double".
 *	Commands resembling some of the commands common to HP scientific
 *	calculators have been added as well.
 *		- Paul G. Fox, Wed Dec 29 1993
 *
 *	[Math is done in "long double", as of 2012.]
 */
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <math.h>

#define DEBUG 1
#undef DEBUG

#ifdef DEBUG
#define debug(a) printf a 
#else
#define debug(a)
#endif

typedef int boolean;
#define TRUE 1
#define FALSE 0

typedef int opreturn;
#define GOODOP 1
#define BADOP 0

typedef long double ldouble;

ldouble pi;

/* internal representation of numbers */
struct num {
	ldouble val;
	struct num *next;
};

/* tokens are typed objects -- currently numbers, operators, line-ends */
struct token {
	union {
		ldouble val;
		opreturn (*opfunc)(struct token *);
		char *str;
	} val;
	int type;
};

typedef struct token token;

/* values for token.type */
#define FLOAT 0
#define OP 1
#define EOL 2
#define UNKNOWN -1

/* all user input is either a number or a command operator --
	this is how the operators are looked up, by name */
struct oper {
	char *name;
	opreturn (*func)(token *);
	char *help;
};

/* prototypes */
void errexit(char *s);
void push(ldouble n);
boolean pop(ldouble *f);
opreturn enter(token *t);
opreturn add(token *t);
opreturn subtract(token *t);
opreturn multiply(token *t);
opreturn divide(token *t);
opreturn y_to_the_x(token *t);
opreturn modulus(token *t);
opreturn rshift(token *t);
opreturn lshift(token *t);
opreturn and(token *t);
opreturn or(token *t);
opreturn xor(token *t);
opreturn not(token *t);
opreturn chsign(token *t);
opreturn recip(token *t);
opreturn squarert(token *t);
opreturn sine(token *t);
opreturn cosine(token *t);
opreturn tangent(token *t);
void printtop(void);
void printstack(void);
opreturn printall(token *t);
opreturn printone(token *t);
opreturn printhex(token *t);
opreturn printoct(token *t);
opreturn printdec(token *t);
opreturn modehex(token *t);
opreturn modeoct(token *t);
opreturn modedec(token *t);
opreturn modeinteger(token *t);
opreturn modefloat(token *t);
opreturn clear(token *t);
opreturn rolldown(token *t);
opreturn repush(token *t);
opreturn exchange(token *t);
opreturn push_pi(token *t);
opreturn autop(token *t);
opreturn quit(token *t);
opreturn help(token *t);
token *gettoken(FILE *f);
void options(int argc, char *argv[]);

/* the operand stack */
struct num *stack;

/* if true, print the top of stack after every user newline */
boolean autoprint = TRUE;

/* decimal, hex, or octal output */
int pformat = 'd';
int oformat = 'd';  /* previous value */

/* integer or float math */
int math = 'f';

/* suppress "empty stack" messages */
boolean silent = FALSE;

/* the most recent top-of-stack */
ldouble lastx;

char *progname = "";

struct oper opers[] = {
	{"+", add, 		"add top two numbers" },
	{"-", subtract, 	"subtract top two numbers" },
	{"*", multiply,		"multiply top two numbers" },
	{"/", divide, 		"divide top two numbers" },
	{"%", modulus, 		"take modulo of top two numbers" },
	{">>", rshift, 		"right shift" },
	{"<<", lshift, 		"left shift" },
	{"&", and, 		"bitwise and" },
	{"|", or, 		"bitwise or" },
	{"xor", xor, 		"bitwise xor" },
	{"~", not, 		"bitwise not" },
	{"", 0, 0},
	{"changesign", chsign,	"negate top number" },
	{"chs", chsign,		" \" \"" },
	{"reciprocal", recip,	"take reciprocal of top number" },
	{"squareroot", squarert,"take square root of top number" },
	{"sqrt", squarert,	" \" \"" },
	{"sin", sine,		"take sine of angle (in degrees)" },
	{"cos", cosine,		"take cosine of angle (in degrees)" },
	{"tan", tangent,	"take tangent of angle (in degrees)" },
	{"^", y_to_the_x, 	"raise next-to-top to power of top" },
	{"raise", y_to_the_x, 	" \" \"" },
	{"", 0, 0},
	{"Print", printall, 	"print whole stack" },
	{"print", printone, 	"print top of stack" },
	{"dprint", printdec, 	"print top of stack in decimal" },
	{"oprint", printoct, 	"print top of stack in octal" },
	{"hprint", printhex, 	"print top of stack in hex" },
	{"xprint", printhex, 	" \" \"" },
	{"", 0, 0},
	{"clear", clear, 	"clear whole stack" },
	{"pop", rolldown, 	"pop (and discard) top of stack" },
	{"push", enter, 	"push (duplicate) top of stack" },
	{"enter", enter, 	" \" \"" },
	{"lastx", repush, 	"re-push most recent previous top of stack" },
	{"lx", repush, 		" \" \"" },
	{"repush", repush, 	" \" \"" },
	{"xchange", exchange, 	"exchange top two numbers" },
	{"exchange", exchange, 	" \" \"" },
	{"pi", push_pi, 	"push constant pi" },
	{"", 0, 0},
	{"Autoprint", autop,	"toggle autoprinting" },
	{"Hex", modehex, 	"switch to hex output" },
	{"X", modehex, 		" \" \"" },
	{"Octal", modeoct, 	"switch to octal output" },
	{"Decimal", modedec, 	"switch to decimal output" },
	{"Integer", modeinteger,"switch to integer arithmetic" },
	{"Float", modefloat, 	"switch to float arithmetic" },
	{"", 0, 0},
	{"quit", quit, 		"leave" },
	{"exit", quit, 		" \" \"" },
	{"", 0, 0},
	{"help", help, 		NULL },
	{"?", help, 		NULL },
	{NULL, NULL}
};


int
main(argc,argv)
int argc;
char *argv[];
{
	token *t;
	static int lasttoktype;

	options(argc,argv);

	pi = 4 * atan(1.0);

	/* we simply loop forever, either pushing operands or executing
		operators.  the special end-of-line token lets us do
		reasonable autoprinting, the last thing on the line was
		an operator */
	while ((t = gettoken(stdin)) != NULL) {
		debug(("got token\n"));
		switch(t->type) {
		case FLOAT:
			debug(("pushing %Lg\n", t->val.val));
			push(t->val.val);
			break;
		case OP:
			debug(("calling op\n"));
			(void)(t->val.opfunc)(t);
			break;
		case EOL:
			if (autoprint && lasttoktype == OP) {
				silent = TRUE;
				printtop();
				silent = FALSE;
			}
			break;
		default:
		case UNKNOWN:
			printf("unrecognized input '%s'\n",t->val.str);
			break;

		}
		lasttoktype = t->type;
	}
	exit(1);
	return (1);
}

void
options(argc,argv)
int argc;
char *argv[];
{
	progname = strrchr(argv[0],'/');
	if (!progname) progname = argv[0];
	if (argc > 1) {
		if (!strcmp(argv[1], "-p"))
			autoprint = !autoprint;
		else {
			fprintf(stderr, "usage: %s [-p] (to turn off autoprinting)\n",
					progname);
			exit(1);
		}
	}
}

token *
gettoken(FILE *f)
{
	static char inputline[1024];
	static struct token tok;
	ldouble n;
	static char *p = NULL;
	static char *t = NULL;
	static char *c = NULL;

	if (p == NULL) {
		if (fgets(inputline, 1024, f) == NULL)
			return NULL;
		p = inputline;
	}
	while (isspace(*p))
		p++;

	debug(("gettoken string is %s\n", p));

	if (*p == '\0') { /* out of input */
		tok.type = EOL;
		p = NULL;
		return &tok;
	}

	/* find end of token */
	t = p;
	while (!isspace(*p))
		p++;

	/* if we're on whitespace, null terminate and increment */
	if (*p) 
		*p++ = '\0';

	/* eliminate commas */
	debug(("precomma %s\n", t));
	while ((c = strchr(t, ',')) != NULL) {
	    memmove(c, c+1, strlen(c));
	}
	debug(("postcomma %s\n", t));


	/* is it a number? */
	if (*t == '0' && (*(t+1) == 'x' || *(t+1) == 'X')) {
		long long ln = strtoll(t, 0, 16);
		tok.type = FLOAT;
		tok.val.val = ln;
		debug(("gettoken hex value is %Lg decimal\n", n));
		return &tok;

	} else if (*t == '0' && ('0' <= *(t+1) && *(t+1) <= '7')) {
		long long ln = strtoll(t, 0, 8);
		tok.type = FLOAT;
		tok.val.val = ln;
		debug(("gettoken octal value is %Lg decimal\n", n));
		return &tok;

	} else if (isdigit(*t) || (*t == '.') ||
			(*t == '-' && isdigit(*(t+1)))) {
	debug(("x postcomma %s\n", t));
		if (sscanf(t,"%Lg",&n) == 1) {
			tok.type = FLOAT;
			tok.val.val = n;
			debug(("gettoken value is %Lg\n", n));
			return &tok;
		}
	} else { /* is it a command? */
		struct oper *op;
		op = opers;
		while (op->name) {
			if (*op->name && !strncmp(t, op->name, strlen(t))) {
				tok.type = OP;
				tok.val.opfunc = op->func;
				debug(("gettoken op is %s\n", op->name));
				return &tok;
			}
			op++;
		}
	}
	tok.val.str = t;
	tok.type = UNKNOWN;
	debug(("gettoken unknown: %s\n", t));
	p = NULL;
	fflush(stdin);
	return &tok;
}

void
push(ldouble n)
{
	struct num *p = (struct num *)calloc(1, sizeof (struct num));
	if (!p) errexit("no memory for push");
	if (math == 'i')
	    p->val = (long long)n;
	else
	    p->val = n;
	p->next = stack;
	stack = p;
}

boolean
pop(ldouble *f)
{
	struct num *p;
	p = stack;
	if (!p) {
		if (!silent)
			printf("empty stack\n");
		return FALSE;
	}
	*f = p->val;
	stack = p->next;
	free (p);
	return TRUE;
}

opreturn
enter( token *t )
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
add( token *t )
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
subtract( token *t )
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
multiply( token *t )
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
divide( token *t )
{
	ldouble a, b;
	if (pop(&b)) {
		if (pop(&a)) {
			if (b != 0.0) {
				push(a / b);
			} else {
				push(a);
				push(b);
				printf("would divide by zero\n");
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
modulus( token *t )
{
	ldouble a, b;
	if (pop(&b)) {
		if (pop(&a)) {
			int i, j;
			i = (long long)a;
			j = (long long)b;
			if (j != 0) {
				push(i / j);
			} else {
				push(a);
				push(b);
				printf("would divide by zero\n");
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
rshift( token *t )
{
	ldouble a, b;
	if (pop(&b)) {
		if (pop(&a)) {
			int i, j;
			i = (long long)a;
			j = (long long)b;
			push(i >> j);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
lshift( token *t )
{
	ldouble a, b;
	if (pop(&b)) {
		if (pop(&a)) {
			int i, j;
			i = (long long)a;
			j = (long long)b;
			push(i << j);
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

opreturn
and( token *t )
{
	ldouble a, b;
	if (pop(&b)) {
		if (pop(&a)) {
			int i, j;
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
or( token *t )
{
	ldouble a, b;
	if (pop(&b)) {
		if (pop(&a)) {
			int i, j;
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
xor( token *t )
{
	ldouble a, b;
	if (pop(&b)) {
		if (pop(&a)) {
			int i, j;
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
not( token *t )
{
	ldouble a;
	if (pop(&a)) {
		push( ~(long long)a );
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
chsign( token *t )
{
	ldouble a;
	if (pop(&a)) {
		push( -a );
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
recip ( token *t )
{
	ldouble a;
	if (pop(&a)) {
		if (a != 0.0) {
			push( 1.0 / a );
			lastx = a;
			return GOODOP;
		} else {
			push(a);
			printf("would divide by zero\n");
			return BADOP;
		}
	}
	return BADOP;
}

opreturn
squarert ( token *t )
{
	ldouble a;
	if (pop(&a)) {
		if (a >= 0.0) {
			push( sqrt(a) );
			lastx = a;
			return GOODOP;
		} else {
			push(a);
			printf("can't take root of negative\n");
			return BADOP;
		}
	}
	return BADOP;
}

opreturn
sine ( token *t )
{
	ldouble a;


	if (pop(&a)) {
		push( sin((a * 2 * pi) / 360.0 ) );
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
cosine ( token *t )
{
	ldouble a;

	if (pop(&a)) {
		push( cos((a * 2 * pi) / 360.0 ) );
		lastx = a;
		return GOODOP;
	}
	return BADOP;
}

opreturn
tangent ( token *t )
{
	ldouble a;

	if (pop(&a)) {
#if LATER
		if (((a + 90) % 180) == 0) {
			push(a);
			printf("can't take tan of +/90\n");
			return BADOP;
		} else
#endif
		{
			push( tan((a * 2 * pi) / 360.0 ) );
			lastx = a;
			return GOODOP;
		}
	}
	return BADOP;
}

opreturn
y_to_the_x ( token *t )
{
	ldouble a, b;
	if (pop(&b)) {
		if (pop(&a)) {
			if (a >= 0 || floor(b) == b) {
				push(pow(a, b));
			} else {
				push(a);
				push(b);
				printf("result would be complex\n");
				return BADOP;
			}
			lastx = b;
			return GOODOP;
		}
		push(b);
	}
	return BADOP;
}

void
printtop (void)
{
	ldouble n;
	long long ln;
	if (pop(&n)) {
		switch (pformat) {
		case 'x':
		    ln = n;
		    printf(" 0x%Lx\n",ln);
		    break;
		case 'o':
		    ln = n;
		    printf(" 0%Lo\n",ln);
		    break;
		default:
		    printf(" %Lg\n",n);
		}
		push(n);
	}
	pformat = oformat;
}

void
printstack (void)
{
	ldouble n;
	if (pop(&n)) {
		(void)printstack();
		push(n);
		printtop();
	}
}

opreturn
printall ( token *t )
{
	ldouble hold;
	silent = TRUE;
	if (autoprint) {
		if (pop(&hold))
			printstack();
		push(hold);
	} else {
		printstack();
	}
	silent = FALSE;
	return GOODOP;
}

opreturn
printone ( token *t )
{
	if (!autoprint) {
		silent = TRUE;
		printtop();
		silent = FALSE;
	}
	return GOODOP;
}

opreturn
printhex ( token *t )
{
	oformat = pformat;
	pformat = 'x';
	printone(t);
	return GOODOP;
}

opreturn
printoct ( token *t )
{
	oformat = pformat;
	pformat = 'o';
	printone(t);
	return GOODOP;
}

opreturn
printdec ( token *t )
{
	oformat = pformat;
	pformat = 'd';
	printone(t);
	return GOODOP;
}

opreturn
modehex ( token *t )
{
	oformat = pformat = 'x';
	printf("output mode is now hex\n");
	if (math != 'i')
		printf("warning: floating math: values not truncated\n");
	return printall(t);
}

opreturn
modeoct ( token *t )
{
	oformat = pformat = 'o';
	printf("output mode is now octal\n");
	if (math != 'i')
		printf("warning: floating math: values not truncated\n");
	return printall(t);
}

opreturn
modedec ( token *t )
{
	oformat = pformat = 'd';
	printf("output mode is now decimal\n");
	return printall(t);
}

opreturn
modeinteger ( token *t )
{
	math = 'i';
	printf("arithmetic mode is now integer\n");
	return printall(t);
}

opreturn
modefloat ( token *t )
{
	math = 'f';
	printf("arithmetic mode is now integer\n");
	return printall(t);
}

opreturn
clear ( token *t )
{
	ldouble scrap;
	if (pop(&lastx)) {
		while (pop(&scrap))
			;
	}
	return GOODOP;
}

opreturn
rolldown ( token *t )
{
	(void) pop(&lastx);
	return GOODOP;
}

opreturn
repush ( token *t )
{
	push(lastx);
	return GOODOP;
}

opreturn
exchange( token *t )
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

opreturn
push_pi ( token *t )
{
	push(pi);
}

opreturn
autop ( token *t )
{
	autoprint = !autoprint;
	printf("autoprinting is now %s\n", autoprint ? "on" : "off");
	return GOODOP;
}

opreturn
quit ( token *t )
{
	exit(0);
}

opreturn
help ( token *t )
{
	struct oper *op;
	extern struct oper opers[];
	op = opers;
	while (op->name) {
		if (!*op->name)
		    putchar('\n');
		else
		    printf("%-20s%s\n", op->name, op->help);
		op++;
	}
	return GOODOP;
}

void
errexit(char *s)
{
	fprintf(stderr, "%s: %s\n", progname, s);
	exit(1);
}

