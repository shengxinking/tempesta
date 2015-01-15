/**
 *		Tempesta FW
 *
 * Tempesta FW Configuration Framework.
 *
 * Requirements:
 *  - The configuring process must be habitual for any system administrator.
 *  - An ability to specify relatively complex configuration entities
 *    (lists, dictionaries, trees, etc).
 *  - Decomposition into modules. Other Tempesta subsystems should be able to
 *    register their sections in a configuration file. That should be possible
 *    for other kernel modules as well, so late binding has to be used.
 *  - Configuration refresh in run time (at least partially).
 *  - An ability to manage very large lists (e.g. blocked IP addresses).
 *
 *  None of existing approaches (sysfs, configfs, sysctl, ioctl) mets all the
 *  requirements, so we implement our own configuration subsystem. Current
 *  implementation doesn't fit all requirements either, but it is flexible
 *  enough for future extensions.
 *
 *  Basically, we store configuration in plain-text files and read them via VFS
 *  and parse right in the kernel space. This is not very conventional, but
 *  allows to pass relatively complex data structures to the kernel.
 *
 *  The configuration looks like this:
 *    entry1 42;
 *    entry2 1 2 3 foo=bar;
 *    entry3 {
 *        sub_entry1;
 *        sub_entry2;
 *    }
 *    entry4 with_value {
 *       and_subentries {
 *           and_subsubentries;
 *       }
 *    }
 *  It consists of entries. Each entry has:
 *    1. name;
 *    2. values (usually just one, but variable number of values is supported);
 *    3. attributes (a dictionary of key-value pairs);
 *    4. children entries (such entries act as sections or trees);
 *  The only name is required. Everything else is optional. The idea is similar
 *  to SDL (http://www.ikayzo.org/display/SDL/Language+Guide), but our syntax
 *  and terminology is more habitual for C/Linux programmers and users.
 *
 *  Tempesta FW modules register themselves and provide their configuration
 *  specifications via TfwCfgMod and TfwCfgSpec structures. The code here pushes
 *  events and parsed configuration via callbacks specified in these structures.
 *
 *  The code in this unit contains two main entities:
 *    1. The configuration parser.
 *       We utilize FSM approach for the parser. The code is divided into two
 *       FSMs: TFSM (tokenizer) and PFSM (the parser that produces entries).
 *    2. A bunch of generic  TfwCfgSpec->handler callbacks for the parser.
 *    3. TfwCfgMod list related routines, the top-level parsing routine.
 *       This part of code implements publishing start/stop events and parsed
 *       configuration data across modules.
 *    4. The list of registered modules, VFS and sysctl helpers, kernel module
 *       parameters. The stateful part of code.
 *
 * TODO:
 *  - "include" directives.
 *  - Handling large sets of data, possibly via TDB.
 *  - Re-loading some parts of the configuration on the fly without stopping the
 *    whole system.
 *  - Verbose error reporting: include file/line and expected/got messages.
 *  - Improve efficiency: too many memory allocations and data copying.
 *
 * Copyright (C) 2012-2014 NatSys Lab. (info@natsys-lab.com).
 * Copyright (C) 2015 Tempesta Technologies.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <linux/ctype.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <net/net_namespace.h> /* for sysctl */

#include "addr.h"
#include "lib.h"
#include "log.h"

#include "cfg.h"

/* FSM's debug messages are very verbose, so they are turned off by default. */
#ifdef DEBUG_CFG_FSM
#define FSM_DBG(...) TFW_DBG(__VA_ARGS__)
#else
#define FSM_DBG(...)
#endif

/* TFSM is even more verbose, it prints a message for every single character,
 * so it is turned on separately. */
#ifdef DEBUG_CFG_TFSM
#define TFSM_DBG(...) TFW_DBG(__VA_ARGS__)
#else
#define TFSM_DBG(...)
#endif

/*
 * ------------------------------------------------------------------------
 *	Configuration Parser - TfwCfgEntry helpers
 * ------------------------------------------------------------------------
 *
 * TfwCfgEntry is a temporary structure that servers only as an interface
 * between the parser and TfwCfgSpec->handler callbacks.
 * The parser walks over input entries accumulating data in the TfwCfgEntry
 * structure. As soon as an entry is parsed, the parser invokes the handler
 * callback and then destroys the TfwCfgEntry object.
 *
 * Strings in the TfwCfgEntry are pieces of the input plain-text configuration,
 * but they have to be NULL-terminated, so we have to allocate space and copy
 * them. Helpers below facilitate that.
 */

static const char *
alloc_and_copy_str(const char *src, size_t len)
{
	char *s;

	BUG_ON(!src);

	s = kmalloc(len + 1, GFP_KERNEL);
	if (!s) {
		TFW_ERR("can't allocate memory\n");
		return NULL;
	}

	memcpy(s, src, len);
	s[len] = '\0';

	return s;
}

/**
 * Check name of an attribute or name
 *
 * Much like C identifiers, names must start with a letter and consist
 * only of alphanumeric and underscore characters. Currently this is
 * only a sanity check and the parser code would work without it, but in
 * future it may help to preserve compatibility if we decide to change
 * the parser.
 */
static bool
check_identifier(const char *buf, size_t len)
{
	size_t i;

	if (!len) {
		TFW_ERR("the string is empty\n");
		return false;
	}

	if (!isalpha(buf[0])) {
		TFW_ERR("the first character is not a letter: '%c'\n", buf[0]);
		return false;
	}

	for (i = 0; i < len; ++i) {
		if (!isalnum(buf[i]) && buf[i] != '_') {
			TFW_ERR("invalid character: '%c' in '%.*s'\n",
				buf[i], (int)len, buf);
			return false;
		}
	}

	return true;
}

static void
entry_reset(TfwCfgEntry *e)
{
	const char *key, *val;
	size_t i;

	BUG_ON(!e);

	kfree(e->name);

	TFW_CFG_ENTRY_FOR_EACH_VAL(e, i, val)
		kfree(val);

	TFW_CFG_ENTRY_FOR_EACH_ATTR(e, i, key, val) {
		kfree(key);
		kfree(val);
	}

	memset(e, 0, sizeof(*e));
}

static int
entry_set_name(TfwCfgEntry *e, const char *name_src, size_t name_len)
{
	BUG_ON(!e || !name_src || !name_len);
	BUG_ON(e->name);

	if (!check_identifier(name_src, name_len))
		return -EINVAL;

	e->name = alloc_and_copy_str(name_src, name_len);
	if (!e->name)
		return -ENOMEM;

	return 0;
}

static int
entry_add_val(TfwCfgEntry *e, const char *val_src, size_t val_len)
{
	const char *val;

	BUG_ON(!e || !val_src || e->val_n > ARRAY_SIZE(e->vals));

	if (e->val_n == ARRAY_SIZE(e->vals)) {
		TFW_ERR("maximum number of values per entry reached\n");
		return -ENOBUFS;
	}

	val = alloc_and_copy_str(val_src, val_len);
	if (!val)
		return -ENOMEM;

	e->vals[e->val_n++] = val;
	return 0;
}

static int
entry_add_attr(TfwCfgEntry *e, const char *key_src, size_t key_len,
		const char *val_src, size_t val_len)
{
	const char *key, *val;

	BUG_ON(!e || !key_src || !val_src);
	BUG_ON(!key_len); /* Although empty values are allowed. */
	BUG_ON(e->attr_n > ARRAY_SIZE(e->attrs));

	if (e->attr_n == ARRAY_SIZE(e->attrs)) {
		TFW_ERR("maximum numer of attributes per entry reached\n");
		return -ENOBUFS;
	}

	if (!check_identifier(key_src, key_len))
		return -EINVAL;

	key = alloc_and_copy_str(key_src, key_len);
	val = alloc_and_copy_str(val_src, val_len);

	if (!key || !val) {
		kfree(key);
		kfree(val);
		return -ENOMEM;
	}

	e->attrs[e->attr_n].key = key;
	e->attrs[e->attr_n].val = val;
	++e->attr_n;
	return 0;
}

/*
 * ------------------------------------------------------------------------
 *	Configuration parser - tokenizer and parser FSMs
 * ------------------------------------------------------------------------
 *
 * Basic terms used in this code:
 *   - MOVE - change FSM state and read the next character/token.
 *   - JMP  - change the state without reading anything.
 *   - SKIP - read the next character/token and re-enter the current state.
 *   - TURN - enter a new state (not re-enter the current state).
 *   - COND_JMP/COND_MOVE/COND_SKIP/etc - do it if the given condition is true.
 *   - lexeme - a sequence of characters in the input buffer.
 *   - token - type/class of a lexeme.
 *   - literal - a lexeme that carries a string value. Regular tokens are syntax
 *               elements, they don't have a value and their lexemes are always
 *               special control characters. Literals are not part of the syntax
 *               and they do have a value.
 *
 * Macro ownership:
 *   - FSM_*() - generic macros shared between PFSM and TFSM.
 *   - PFSM_*()/TFSM_*() - macros specific to parser/tokenizer.
 * For example, FSM_STATE() and FSM_JMP() are generic, they do the same thing
 * in both FSMs (a label and a jump to it); but PFSM_MOVE() and TFSM_MOVE() are
 * different, since they read values from different input streams (tokens and
 * characters respectively).
 */

typedef enum {
	TOKEN_NA = 0,
	TOKEN_LBRACE,
	TOKEN_RBRACE,
	TOKEN_EQSIGN,
	TOKEN_SEMICOLON,
	TOKEN_LITERAL,
	_TOKEN_COUNT,
} token_t;

typedef struct {
	const char  *in;      /* The whole input buffer. */
	const char  *pos;     /* Current position in the @in buffer. */

	/* Current FSM state is saved to here. */
	const void  *fsm_s;   /* Pointer to label (GCC extension). */
	const char  *fsm_ss;  /* Label name as string (for debugging). */

	/* Currently/previously processed character. */
	char        c;
	char        prev_c;

	/* Currently/previously processed token.
	 * The language is context-sensitive, so we need to store all these
	 * previous tokens and literals to parse it without peek()'ing. */
	token_t t;
	token_t prev_t;

	/* Literal value (not NULL only when @t == TOKEN_LITERAL). */
	const char *lit;
	const char *prev_lit;

	/* Length of @lit (the @lit is not terminated). */
	int lit_len;
	int prev_lit_len;

	int  err;  /* The latest error code. */

	/* Currently parsed entry. Accumulates literals as values/attributes.
	 * When current entry is done, a TfwCfgSpec->handler is called and a new
	 * entry is started. */
	TfwCfgEntry e;
} TfwCfgParserState;

/* Macros common for both TFSM and PFSM. */

#define FSM_STATE(name) 		\
	FSM_DBG("fsm: implicit exit from: %s\n", ps->fsm_ss); \
	BUG();				\
name:					\
	if (ps->fsm_s != &&name) {	\
		FSM_DBG("fsm turn: %s -> %s\n", ps->fsm_ss, #name); \
		ps->fsm_s = &&name;	\
		ps->fsm_ss = #name;	\
	}

#define FSM_JMP(to_state) goto to_state

#define FSM_COND_JMP(cond, to_state) \
	FSM_COND_LAMBDA(cond, FSM_JMP(to_state))

#define FSM_COND_LAMBDA(cond, ...)	\
do {					\
	if (cond) {			\
		__VA_ARGS__;		\
	}				\
} while (0)				\

/* Macros specific to TFSM. */

#define TFSM_MOVE(to_state)	\
do {				\
	ps->prev_c = ps->c;	\
	ps->c = *(++ps->pos);	\
	TFSM_DBG("tfsm move: '%c' -> '%c'\n", ps->prev_c, ps->c); \
	FSM_JMP(to_state);	\
} while (0)

#define TFSM_MOVE_EXIT(token_type)	\
do {					\
	ps->t = token_type;		\
	TFSM_MOVE(TS_EXIT);		\
} while (0)

#define TFSM_JMP_EXIT(token_type)	\
do {					\
	ps->t = token_type;		\
	FSM_JMP(TS_EXIT);		\
} while (0)

#define TFSM_SKIP() TFSM_MOVE(*ps->fsm_s);

#define TFSM_COND_SKIP(cond) \
	FSM_COND_LAMBDA(cond, TFSM_SKIP())

#define TFSM_COND_MOVE_EXIT(cond, token_type) \
	FSM_COND_LAMBDA(cond, TFSM_MOVE_EXIT(token_type))

#define TFSM_COND_JMP_EXIT(cond, token_type) \
	FSM_COND_LAMBDA(cond, TFSM_JMP_EXIT(token_type))

#define TFSM_COND_MOVE(cond, to_state) \
	FSM_COND_LAMBDA(cond, TFSM_MOVE(to_state))

/* Macros specific to PFSM. */

#define PFSM_MOVE(to_state)					\
do {								\
	read_next_token(ps);					\
	FSM_DBG("pfsm move: %d (\"%.*s\") -> %d (\"%.*s\")", 	\
		ps->prev_t, ps->prev_lit_len, ps->prev_lit,  	\
		ps->t, ps->lit_len, ps->lit); 			\
	if(!ps->t) {						\
		ps->err = -EINVAL;				\
		FSM_JMP(PS_EXIT);				\
	}							\
	FSM_JMP(to_state);					\
} while (0)

#define PFSM_COND_MOVE(cond, to_state) \
	FSM_COND_LAMBDA(cond, PFSM_MOVE(to_state))

/**
 * The TFSM (Tokenizer Finitie State Machine).
 *
 * Steps over characters in the input stream and classifies them as tokens.
 * Eats whitespace and comments automatically, never produces tokens for them.
 * Accumulates string literals in @ps->lit.
 * Produces one token per call (puts it to @ps->t), shifts current position
 * accordingly. Produces TOKEN_NA on EOF or invalid input.
 */
static void
read_next_token(TfwCfgParserState *ps)
{
	ps->prev_t = ps->t;
	ps->prev_lit = ps->lit;
	ps->prev_lit_len = ps->lit_len;
	ps->lit = NULL;
	ps->lit_len = 0;
	ps->t = TOKEN_NA;
	ps->c = *ps->pos;

	FSM_DBG("tfsm start, char: '%c', pos: %.20s\n", ps->c, ps->pos);

	FSM_JMP(TS_START_NEW_TOKEN);

	/* The next character is read at _TFSM_MOVE(), so we have a fresh
	 * character automatically whenever we enter a state. */

	FSM_STATE(TS_START_NEW_TOKEN) {
		TFSM_COND_JMP_EXIT(!ps->c, TOKEN_NA);

		/* A backslash means that the next character definitely has
		 * no special meaning and thus starts a literal. */
		TFSM_COND_MOVE(ps->c == '\\', TS_LITERAL_FIRST_CHAR);

		/* Eat non-escaped spaces. */
		TFSM_COND_SKIP(isspace(ps->c));

		/* A character next to a double quote is the first character
		 * of a literal. The quote itself is not included to the
		 * literal's value. */
		TFSM_COND_MOVE(ps->c == '"', TS_QUOTED_LITERAL_FIRST_CHAR);

		/* A comment is starts with '#' (and ends with a like break) */
		TFSM_COND_MOVE(ps->c == '#', TS_COMMENT);

		/* Self-meaning single-token characters. */
		TFSM_COND_MOVE_EXIT(ps->c == '{', TOKEN_LBRACE);
		TFSM_COND_MOVE_EXIT(ps->c == '}', TOKEN_RBRACE);
		TFSM_COND_MOVE_EXIT(ps->c == '=', TOKEN_EQSIGN);
		TFSM_COND_MOVE_EXIT(ps->c == ';', TOKEN_SEMICOLON);

		/* Everything else is not a special character and therefore
		 * it starts a literal. */
		FSM_JMP(TS_LITERAL_FIRST_CHAR);
	}

	FSM_STATE(TS_COMMENT) {
		TFSM_COND_JMP_EXIT(!ps->c, TOKEN_NA);

		/* Eat everything until a new line is reached.
		 * The line break cannot be escaped within a comment. */
		TFSM_COND_SKIP(ps->c != '\n');
		TFSM_MOVE(TS_START_NEW_TOKEN);
	}

	FSM_STATE(TS_LITERAL_FIRST_CHAR) {
		ps->lit = ps->pos;
		FSM_JMP(TS_LITERAL_ACCUMULATE);
	}

	FSM_STATE(TS_LITERAL_ACCUMULATE) {
		/* EOF terminates a literal if there is any chars saved. */
		TFSM_COND_JMP_EXIT(!ps->c && !ps->lit_len, TOKEN_NA);
		TFSM_COND_JMP_EXIT(!ps->c && ps->lit_len, TOKEN_LITERAL);

		/* Non-escaped special characters terminate the literal. */
		if (ps->prev_c != '\\') {
			TFSM_COND_JMP_EXIT(isspace(ps->c), TOKEN_LITERAL);
			TFSM_COND_JMP_EXIT(ps->c == '"', TOKEN_LITERAL);
			TFSM_COND_JMP_EXIT(ps->c == '#', TOKEN_LITERAL);
			TFSM_COND_JMP_EXIT(ps->c == '{', TOKEN_LITERAL);
			TFSM_COND_JMP_EXIT(ps->c == '}', TOKEN_LITERAL);
			TFSM_COND_JMP_EXIT(ps->c == ';', TOKEN_LITERAL);
			TFSM_COND_JMP_EXIT(ps->c == '=', TOKEN_LITERAL);
		}

		/* Accumulate everything else. */
		++ps->lit_len;
		TFSM_SKIP();
	}

	FSM_STATE(TS_QUOTED_LITERAL_FIRST_CHAR) {
		ps->lit = ps->pos;
		FSM_JMP(TS_QUOTED_LITERAL_ACCUMULATE);
	}

	FSM_STATE(TS_QUOTED_LITERAL_ACCUMULATE) {
		/* EOF means there is no matching double quote. */
		TFSM_COND_JMP_EXIT(!ps->c, TOKEN_NA);

		/* Only a non-escaped quote terminates the literal. */
		TFSM_COND_MOVE_EXIT(ps->c == '"' && ps->prev_c != '\\', TOKEN_LITERAL);

		/* Everything else is accumulated (including line breaks). */
		++ps->lit_len;
		TFSM_SKIP();
	}

	FSM_STATE(TS_EXIT) {
		FSM_DBG("tfsm exit: t: %d, lit: %.*s\n", ps->t, ps->lit_len, ps->lit);
	}
}

/**
 * The PFSM (Parser Finitie State Machine).
 *
 * Steps over a stream of tokens (produces by the TFSM), accumulates values
 * in TfwCfgEntry and returns it when the input entry is terminated with ';'.
 * Returns one entry at a time and shifts the input position accordingly.
 * Should be called in a loop until NULL is returned.
 *
 * Doesn't recurse into nested entries.
 * I.e. it doesn't fully parse this:
 *   entry1 {
 *       entry2;
 *   }
 * Instead, it stops at the '{' character and the higher-level code has to use
 * push-down automaton approach to parse the section between '{' and '}'.
 * That is done because we are not going to complicate things here by building
 * a large syntax tree and creating a DSL to query it.
 */
static void
parse_cfg_entry(TfwCfgParserState *ps)
{
	FSM_DBG("pfsm: start\n");
	BUG_ON(ps->err);

	/* Start of the input? Read the first token and start a new entry. */
	if (ps->in == ps->pos) {
		read_next_token(ps);
		if (!ps->t)
			FSM_JMP(PS_EXIT);
	}

	/* Continue: start a new entry at the current position. */
	BUG_ON(!ps->t);
	FSM_JMP(PS_START_NEW_ENTRY);

	/* Every _PFSM_MOVE() invokes _read_next_token(), so when we enter
	 * any state, we get a new token automatically.
	 * So:
	 *  name key = value;
	 *  ^
	 *  current literal is here; we need to store it as the name.
	 */
	FSM_STATE(PS_START_NEW_ENTRY) {
		entry_reset(&ps->e);
		FSM_DBG("set name: %.*s\n", ps->lit_len, ps->lit);

		ps->err = entry_set_name(&ps->e, ps->lit, ps->lit_len);
		FSM_COND_JMP(ps->err, PS_EXIT);

		PFSM_MOVE(PS_VAL_OR_ATTR);
	}

	/* The name was easy.
	 * Now we have a situation where at current position we don't know
	 * whether we have a value or an attribute:
	 *     name key = value;
	 *          ^
	 *          current position here
	 *
	 * An implementation of peek_token() would be tricky here because the
	 * TFSM is not pure (it alters the current state). So instead of looking
	 * forward, we move to the next position and look to the '=' sign:
	 * if it is there - then we treat previous value as an attribute name,
	 * otherwise we save it as a value of the current node.
	 */

	FSM_STATE(PS_VAL_OR_ATTR) {
		PFSM_COND_MOVE(ps->t == TOKEN_LITERAL, PS_MAYBE_EQSIGN);
		FSM_COND_JMP(ps->t == TOKEN_SEMICOLON, PS_SEMICOLON);
		FSM_COND_JMP(ps->t == TOKEN_LBRACE, PS_LBRACE);
	}

	FSM_STATE(PS_MAYBE_EQSIGN) {
		FSM_COND_JMP(ps->t == TOKEN_EQSIGN, PS_STORE_ATTR_PREV);
		FSM_JMP(PS_STORE_VAL_PREV);
	}

	FSM_STATE(PS_STORE_VAL_PREV) {
		/* name val1 val2;
		 *           ^
		 *           We are here (but still need to store val1). */
		FSM_DBG("add value: %.*s\n", ps->prev_lit_len, ps->prev_lit);

		ps->err = entry_add_val(&ps->e, ps->prev_lit, ps->prev_lit_len);
		FSM_COND_JMP(ps->err, PS_EXIT);

		FSM_JMP(PS_VAL_OR_ATTR);
	}

	FSM_STATE(PS_STORE_ATTR_PREV) {
		/* name key = val;
		 *          ^
		 *          We are here. */
		const char *key, *val;
		int key_len, val_len;

		key = ps->prev_lit;
		key_len = ps->prev_lit_len;
		read_next_token(ps);  /* eat '=' */
		val = ps->lit;
		val_len = ps->lit_len;

		FSM_DBG("add attr: %.*s = %.*s\n", key_len, key, val_len, val);

		ps->err = entry_add_attr(&ps->e, key, key_len, val, val_len);
		FSM_COND_JMP(ps->err, PS_EXIT);

		PFSM_MOVE(PS_VAL_OR_ATTR);
	}

	FSM_STATE(PS_LBRACE) {
		/* Simply exit on '{' leaving nested nodes untouched and
		 * surrounded with braces. The caller should detect it and parse
		 * them in a loop. */
		ps->e.have_children = true;
		FSM_JMP(PS_EXIT);
	}

	FSM_STATE(PS_SEMICOLON) {
		/* Simply eat ';'. Don't MOVE because the next character may be
		 * '\0' and that triggers an error (because we expect more input
		 * tokens when we do _PFSM_MOVE()). */
		read_next_token(ps);
		FSM_JMP(PS_EXIT);
	}

	FSM_STATE(PS_EXIT) {
		FSM_DBG("pfsm: exit\n");
	}
}

/*
 * ------------------------------------------------------------------------
 *	Configuration Parser - TfwCfgSpec helpers.
 * ------------------------------------------------------------------------
 *
 * The configuration parsing is done slightly differently depending on the
 * context (top-level vs recursing into children entries), but the TfwCfgSpec
 * is handled in the same way in both cases. So the code below is the shared
 * logic between these two cases.
 */

static TfwCfgSpec *
spec_find(TfwCfgSpec specs[], const char *name)
{
	TfwCfgSpec *spec;

	TFW_CFG_FOR_EACH_SPEC(spec, specs) {
		if (!strcmp(spec->name, name))
			return spec;
	}

	return NULL;
}

static void
spec_start_handling(TfwCfgSpec specs[])
{
	TfwCfgSpec *spec;

	TFW_CFG_FOR_EACH_SPEC(spec, specs) {
		spec->call_counter = 0;

		/* Sanity checks. */
		BUG_ON(!spec->name);
		BUG_ON(!*spec->name);
		BUG_ON(!check_identifier(spec->name, strlen(spec->name)));
		BUG_ON(!spec->handler);
		BUG_ON(spec->call_counter < 0);
	}
}

int
spec_handle_entry(TfwCfgSpec *spec, TfwCfgEntry *parsed_entry)
{
	int r;

	if (!spec->allow_repeat && spec->call_counter) {
		TFW_ERR("duplicate entry: '%s', only one such entry is allowed."
			"\n", parsed_entry->name);
		return -EINVAL;
	}

	r = spec->handler(spec, parsed_entry);
	if (r) {
		TFW_ERR("configuration handler returned error: %d\n", r);
		return r;
	}

	++spec->call_counter;
	return 0;
}

/**
 * Handle TfwCfgSpec->deflt . That is done by constructing a buffer containing
 * fake configuration text and parsing it as if it was a real configuration.
 * The parsed TfwCfgEntry then is passed to the TfwCfgSpec->handler as usual.
 *
 * The default value is specified in the source code, so you get a BUG() here
 * if it is not valid.
 *
 * TODO: refactoring. The code is not elegant.
 */
static void
spec_handle_default(TfwCfgSpec *spec)
{
	int len, r;
	static char fake_entry_buf[PAGE_SIZE];
	static TfwCfgParserState ps;

	len = snprintf(fake_entry_buf, sizeof(fake_entry_buf), "%s %s;",
		 spec->name, spec->deflt);
	BUG_ON(len >= sizeof(fake_entry_buf));

	memset(&ps, 0, sizeof(ps));
	ps.in = ps.pos = fake_entry_buf;
	parse_cfg_entry(&ps);
	BUG_ON(!ps.e.name);
	BUG_ON(ps.err);
	BUG_ON(ps.t != TOKEN_NA);

	r = spec_handle_entry(spec, &ps.e);
	BUG_ON(r);
}

static int
spec_finish_handling(TfwCfgSpec specs[])
{
	TfwCfgSpec *spec;

	/* Here we are interested in specs that were not triggered during
	 * the configuration parsing. There are three cases here:
	 *  1. deflt != NULL
	 *     Ok: just use the default value instead of real configuration.
	 *  2. deflt == NULL && allow_none == true
	 *     Ok: no such entry parsed at all (including the default),
	 *     but this is allowed, so do nothing.
	 *  3. deflt == NULL && allow_none == false
	 *     Error: the field is not optional, no such entry parsed and no
	 *     default value is provided, so issue an error.
	 */
	TFW_CFG_FOR_EACH_SPEC(spec, specs) {
		if (!spec->call_counter) {
			if (spec->deflt)
				/* The default value shall not produce error. */
				spec_handle_default(spec);
			else if (!spec->allow_none)
				/* Jump just because TFW_ERR() is ugly here. */
				goto err_no_entry;
		}
	}

	return 0;

err_no_entry:
	TFW_ERR("the required entry is not found: '%s'\n", spec->name);
	return -EINVAL;
}

/*
 * ------------------------------------------------------------------------
 *	Configuration parser - generic TfwCfgSpec->handlers functions
 *	and other helpers for writing custom handlers.
 * ------------------------------------------------------------------------
 */

int
tfw_cfg_map_enum(const TfwCfgEnumMapping mappings[],
		 const char *in_name, void *out_int)
{
	int *out;
	const TfwCfgEnumMapping *pos;

	/* The function writes an int, but usually you want to pass an enum
	 * as the @out_int, so ensure check that their sizes are equal.
	 * Beware: that doesn't protect from packed enums. You may get a memory
	 * corruption if you pass an enum __attribute__((packed)) as @out_int.
	 */
	typedef enum { _DUMMY } _dummy;
	BUILD_BUG_ON(sizeof(_dummy) != sizeof(int));

	BUG_ON(!mappings);
	BUG_ON(!in_name);
	BUG_ON(!out_int);

	if (!check_identifier(in_name, strlen(in_name)))
		return -EINVAL;

	for (pos = mappings; pos->name; ++pos) {
		BUG_ON(!check_identifier(pos->name, strlen(pos->name)));
		if (!strcasecmp(in_name, pos->name)) {
			out = out_int;
			*out = pos->value;
			return 0;
		}
	}

	return -EINVAL;
}
EXPORT_SYMBOL(tfw_cfg_map_enum);

/**
 * Most of the handlers below work with single-value entries like this:
 *   option1 42;
 *   option2 true;
 *   option3 192.168.1.1;
 *
 * This function helps those handlers to check that the input entry matches
 * to the expected pattern: single value, no attributes, no children entries.
 */
int
tfw_cfg_check_single_val(const TfwCfgEntry *e)
{
	int r = -EINVAL;

	if (e->val_n == 0)
		TFW_ERR("no value specified\n");
	else if (e->val_n > 1)
		TFW_ERR("more than one value specified\n");
	else if (e->attr_n)
		TFW_ERR("unexpected attributes\n");
	else if (e->have_children)
		TFW_ERR("unexpected children entries\n");
	else
		r = 0;

	return r;
}
EXPORT_SYMBOL(tfw_cfg_check_single_val);

/**
 * This handler allows to parse nested entries recursively.
 *
 * @arg must be an array of TfwCfgSpec structures which is applied to nested
 * entries.
 *
 * When there are nested entries, the parse_and_handle_cfg_entry()
 * stops at this position:
 *         v
 * section {
 *     option1;
 *     option2;
 *     option3;
 *     ...
 * }
 * ...and invokes the TfwCfgSpec->handler which turns out to be this fucntion.
 * Here we simply continue parsing by recursing to parse_and_handle_cfg_entry().
 *
 * Also, we cheat here: we don't create a new TfwCfgParserState, but rather
 * continue using the parent state. We know that the TfwCfgEntry is the part
 * of the parent state, so we simply restore it with container_of().
 */
int
tfw_cfg_parse_children(TfwCfgSpec *cs, TfwCfgEntry *e)
{
	TfwCfgParserState *ps = container_of(e, TfwCfgParserState, e);
	TfwCfgSpec *nested_specs = cs->dest;
	TfwCfgSpec *matching_spec;
	int r;

	if (e->val_n || e->attr_n) {
		TFW_ERR("the entry must have no values or attributes\n");
		return -EINVAL;
	}
	if (!e->have_children) {
		TFW_ERR("the entry has no nested children entries\n");
		return -EINVAL;
	}

	spec_start_handling(nested_specs);

	/* Eat '{'. */
	BUG_ON(ps->t != TOKEN_LBRACE);
	read_next_token(ps);
	if (ps->err)
		return ps->err;

	/* Walk over children entries. */
	while (ps->t != TOKEN_RBRACE) {
		parse_cfg_entry(ps);
		if (ps->err) {
			TFW_ERR("syntax error\n");
			return ps->err;
		}

		matching_spec = spec_find(nested_specs, ps->e.name);
		if (!matching_spec) {
			TFW_ERR("don't know how to handle: %s\n", ps->e.name);
			return -EINVAL;
		}

		r = spec_handle_entry(matching_spec, &ps->e);
		if (r)
			return r;
	}

	/* Eat '}'. */
	read_next_token(ps);
	if (ps->err)
		return ps->err;

	return spec_finish_handling(nested_specs);
}
EXPORT_SYMBOL(tfw_cfg_parse_children);

int
tfw_cfg_set_bool(TfwCfgSpec *cs, TfwCfgEntry *e)
{
	bool is_true, is_false;
	bool *dest_bool = cs->dest;
	const char *in_str = e->vals[0];

	BUG_ON(!dest_bool);
	if (tfw_cfg_check_single_val(e))
		return -EINVAL;

	is_true =  !strcasecmp(in_str, "1")
	        || !strcasecmp(in_str, "y")
	        || !strcasecmp(in_str, "on")
	        || !strcasecmp(in_str, "yes")
	        || !strcasecmp(in_str, "true")
	        || !strcasecmp(in_str, "enable");

	is_false =  !strcasecmp(in_str, "0")
	         || !strcasecmp(in_str, "n")
	         || !strcasecmp(in_str, "off")
	         || !strcasecmp(in_str, "no")
	         || !strcasecmp(in_str, "false")
	         || !strcasecmp(in_str, "disable");

	BUG_ON(is_true && is_false);
	if (!is_true && !is_false) {
		TFW_ERR("invalid boolean value: '%s'\n", in_str);
		return -EINVAL;
	}

	*dest_bool = is_true;
	return 0;
}
EXPORT_SYMBOL(tfw_cfg_set_bool);

/**
 * Detect integer base and strip 0x and 0b prefixes from the string.
 *
 * The custom function is written because the kstrtox() treats leading zeros as
 * the octal base. That may cause an unexpected effect when you specify "010" in
 * the configuration and get 8 instead of 10. We want to avoid that.
 *
 * As a bonus, we have the "0b" support here. This may be handy for specifying
 * some masks and bit strings in the configuration.
 */
static int
detect_base(const char **pos)
{
	const char *str = *pos;
	size_t len = strlen(str);

	if (!len)
		return 0;

	if (len > 2 && str[0] == '0' && isalpha(str[1])) {
		char c = tolower(str[1]);

		(*pos) += 2;

		if (c == 'x')
			return 16;
		else if (c == 'b')
			return 2;
		else
			return 0;
	}

	return 10;
}

int
tfw_cfg_set_int(TfwCfgSpec *cs, TfwCfgEntry *e)
{
	int base, r, val;
	int *dest_int;
	const char *in_str;


	BUG_ON(!cs->dest);
	r = tfw_cfg_check_single_val(e);
	if (r)
		goto err;

	in_str = e->vals[0];
	base = detect_base(&in_str);
	if (!base)
		goto err;

	r = kstrtoint(in_str, base, &val);
	if (r)
		goto err;

	if (cs->spec_ext) {
		TfwCfgSpecInt *cse = cs->spec_ext;

		if (cse->is_multiple_of && (val % cse->is_multiple_of)) {
			TFW_ERR("the value of '%s' is not a multiple of %d\n",
				in_str ,cse->is_multiple_of);
			goto err;
		}

		if (cse->range.min != cse->range.max &&
		    (val < cse->range.min || val > cse->range.max)) {
			TFW_ERR("the value of '%s' is out of range: %ld, %ld\n",
				in_str, cse->range.min, cse->range.max);
			goto err;
		}
	}

	dest_int = cs->dest;
	*dest_int = val;
	return 0;
err:
	TFW_ERR("can't parse integer");
	return -EINVAL;
}
EXPORT_SYMBOL(tfw_cfg_set_int);

int
tfw_cfg_set_str(TfwCfgSpec *cs, TfwCfgEntry *e)
{
	const char *in_str = e->vals[0];
	const char **dest_str = cs->dest;
	TfwCfgSpecStr *cse = cs->spec_ext;
	int r, len, len_min, len_max;

	r = tfw_cfg_check_single_val(e);
	if (r)
		return r;

	BUG_ON(!dest_str);
	BUG_ON(!cs);
	BUG_ON(!cse->buf.buf || !cse->buf.size); /* TODO: dynamic allocation. */

	len = strlen(in_str);
	if (len >= cse->buf.size) {
		TFW_ERR("the string is too long: '%s'\n", in_str);
		return -EINVAL;
	}

	len_min = cse->len_range.min;
	len_max = cse->len_range.max;
	if (len_min != len_max && (len < len_min || len > len_max)) {
		TFW_ERR("the string length (%d) is out of valid range (%d, %d):"
			" '%s'\n", len, len_min, len_max, in_str);
		return -EINVAL;
	}

	memcpy(cse->buf.buf, in_str, len + 1);
	*dest_str = cse->buf.buf;
	return 0;
}
EXPORT_SYMBOL(tfw_cfg_set_str);

/*
 * ------------------------------------------------------------------------
 *	TfwCfgMod list related routines, the top-level parsing routine.
 * ------------------------------------------------------------------------
 */

#define MOD_FOR_EACH(pos, head) \
	list_for_each_entry(pos, head, list)

#define MOD_FOR_EACH_REVERSE(pos, head) \
	list_for_each_entry_reverse(pos, head, list)

#define MOD_FOR_EACH_SAFE_REVERSE(pos, tmp, head) \
	list_for_each_entry_safe_reverse(pos, tmp, head, list)

/**
 * Iterate over modules in the reverse order starting from an element which
 * is previous to the @curr_pos. Useful for roll-back'ing all previously
 * processed modules when an operation for the current module is failed.
 */
#define MOD_FOR_EACH_REVERSE_FROM_PREV(pos, curr_pos, head) 		\
	for (pos = list_entry(curr_pos->list.prev, TfwCfgMod, list);  	\
	     &pos->list != head; 				\
	     pos = list_entry(pos->list.prev, TfwCfgMod, list))

/**
 * Invoke TfwCfgMod->@callback-name.
 */
#define MOD_CALL(modp, callback_name) 					\
do {									\
	TFW_DBG("mod_%s(): %s\n", #callback_name, (modp)->name);	\
	if ((modp)->callback_name)					\
		(modp)->callback_name();				\
} while (0)

/**
 * Invoke TfwCfgMod->@callback_name and check the return value.
 */
#define MOD_CALL_RET(modp, callback_name)				\
({									\
	int _ret = 0;							\
	TFW_DBG("mod_%s(): %s\n", #callback_name, (modp)->name);	\
	if ((modp)->callback_name)					\
		ret = (modp)->callback_name();				\
	if (_ret) 							\
		TFW_ERR("failed: mod_%s(): %s\n",			\
			 #callback_name, (modp)->name); 		\
	_ret;								\
})

static void
print_parse_error(const TfwCfgParserState *ps)
{
	const char *start = max((ps->pos - 80), ps->in);
	int len = ps->pos - start;

	TFW_ERR("configuration parsing error:\n"
		"%.*s\n"
		"^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^",
		len, start);
}

/**
 * The top-level parsing routine.
 *
 * Parses @cfg_text and pushes the parsed data to all modules in the @mod_list.
 * For each parsed entry searches for a matching TfwCfgSpec across all specs
 * of all modules in the @mod_list.
 */
int
tfw_cfg_parse_mods_cfg(const char *cfg_text, struct list_head *mod_list)
{
	TfwCfgParserState ps = {
		.in = cfg_text,
		.pos = cfg_text
	};
	TfwCfgMod *mod;
	TfwCfgSpec *matching_spec;
	int r = -EINVAL;

	MOD_FOR_EACH(mod, mod_list) {
		if (mod->specs)
			spec_start_handling(mod->specs);
	}

	do {
		parse_cfg_entry(&ps);
		if (ps.err) {
			TFW_ERR("syntax error\n");
			goto err;
		}
		if (!ps.e.name)
			break; /* EOF - nothing is parsed and no error. */

		MOD_FOR_EACH(mod, mod_list) {
			if(!mod->specs)
				continue;
			matching_spec = spec_find(mod->specs, ps.e.name);
			if (matching_spec)
				break;
		}
		if (!matching_spec) {
			TFW_ERR("don't know how to handle: '%s'\n", ps.e.name);
			goto err;
		}

		r = spec_handle_entry(matching_spec, &ps.e);
		if (r)
			goto err;
	} while (ps.t);

	MOD_FOR_EACH(mod, mod_list) {
		if (mod->specs) {
			r = spec_finish_handling(mod->specs);
			if (r)
				goto err;
		}
	}

	return 0;
err:
	print_parse_error(&ps);
	return -EINVAL;
}
DEBUG_EXPORT_SYMBOL(tfw_cfg_parse_mods_cfg);

/**
 * Start all modules, parse the @cfg_text and push the parsed data to modules.
 *
 * The two distinct @setup/@start passes are required to allow setting callbacks
 * that are executed both before and after configuration parsing.
 *
 * Upon error, the function tries to roll-back the state: if any modules are
 * already started, it stops them and so on.
 */
static int
tfw_cfg_start_mods(const char *cfg_text, struct list_head *mod_list)
{
	int ret;
	TfwCfgMod *mod, *tmp_mod;

	BUG_ON(list_empty(mod_list));

	TFW_DBG("setting up modules...\n");
	MOD_FOR_EACH(mod, mod_list) {
		ret = MOD_CALL_RET(mod, setup);
		if (ret)
			goto err_recover_cleanup;
	}

	TFW_DBG("parsing configuration and pushing it to modules...\n");
	ret = tfw_cfg_parse_mods_cfg(cfg_text, mod_list);
	if (ret) {
		TFW_ERR("can't parse configuration data\n");
		goto err_recover_cleanup;
	}

	TFW_DBG("starting modules...\n");
	MOD_FOR_EACH(mod, mod_list) {
		ret = MOD_CALL_RET(mod, start);
		if (ret)
			goto err_recover_stop;
	}

	TFW_LOG("modules are started\n");
	return 0;

err_recover_stop:
	TFW_DBG("stopping already stared modules\n");
	MOD_FOR_EACH_REVERSE_FROM_PREV(tmp_mod, mod, mod_list) {
		MOD_CALL(tmp_mod, stop);
	}

err_recover_cleanup:
	TFW_DBG("cleaning up already initialized modules\n");
	MOD_FOR_EACH_REVERSE_FROM_PREV(tmp_mod, mod, mod_list) {
		MOD_CALL(tmp_mod, cleanup);
	}

	return ret;
}

/**
 * Stop all registered modules.
 *
 * That is done in two passes:
 * 1. Invoke "stop" callback for all modules.
 * 2. Invoke "cleanup" callback for all modules.
 *
 * The two distinct passes are needed to avoid synchronization.
 * Modules may reference each other, so if some work is happening during the
 * execution of this function, it may cause running modules to reference already
 * stopped and un-initialized modules.To avoid that, we introduce the "cleanup"
 * pass where modules may free memory after everything is stopped.
 *
 * Passes are done in reverse order of tfw_cfg_mod_start_all()
 * (modules are started/stopped in LIFO manner).
 */
static void
tfw_cfg_stop_mods(struct list_head *mod_list)
{
	TfwCfgMod *mod;

	TFW_DBG("stopping modules...\n");

	MOD_FOR_EACH_REVERSE(mod, mod_list) {
		MOD_CALL(mod, stop);
	}

	MOD_FOR_EACH_REVERSE(mod, mod_list) {
		MOD_CALL(mod, cleanup);
	}

}

/*
 * ------------------------------------------------------------------------
 *	The list of registered modules, VFS and sysctl helpers.
 * ------------------------------------------------------------------------
 */

/* The buffer net.tempesta.state value as a string.
 * We need to store it to avoid double start or stop action.*/
static char tfw_cfg_sysctl_state_buf[32];

/* The file path is passed via the kernel module parameter.
 * Usually you would not like to change it on a running system. */
static char *tfw_cfg_path = "/etc/tempesta.conf";
module_param(tfw_cfg_path, charp, 0444);
MODULE_PARM_DESC(tfw_cfg_path, "Path to Tempesta FW configuration file.");

/* The global list of all registered modules (consists of TfwCfgMod objects). */
static LIST_HEAD(tfw_cfg_mods);

/* The serialized value of tfw_cfg_sysctl_state_buf.
 * Indicates that all registered modules are started. */
bool tfw_cfg_mods_are_started;

/**
 * Read the whole file and put all the contents to the @out_buf.
 */
static int
read_file_via_vfs(const char *path, char *out_buf, size_t buf_size)
{
	struct file *fp;
	mm_segment_t oldfs;
	loff_t offset = 0;
	size_t bytes_read, read_size;
	int ret = 0;

	TFW_DBG("reading file: %s\n", path);

	--buf_size; /* reserve one bye for '\0'. */
	oldfs = get_fs();
	set_fs(get_ds());

	fp = filp_open(path, O_RDONLY, 0);
	if (IS_ERR_OR_NULL(fp)) {
		TFW_ERR("can't open file: %s (err: %ld)\n", path, PTR_ERR(fp));
		goto out;
	}

	do {
		buf_size -= offset;
		read_size = min(buf_size, PAGE_SIZE);
		bytes_read = vfs_read(fp, out_buf + offset, read_size, \
				      &offset);
		if (bytes_read < 0) {
			ret = bytes_read;
			TFW_ERR("can't read file: %s (err: %d)\n", path, ret);
			goto out;
		}
	} while (bytes_read);

out:
	if (!IS_ERR_OR_NULL(fp))
		filp_close(fp, NULL);
	set_fs(oldfs);
	out_buf[offset] = '\0';

	return ret;
}

/**
 * Process command received from sysctl as string (either "start" or "stop").
 * Do corresponding actions, but only if the state is changed.
 */
static int
handle_state_change(const char *old_state, const char *new_state)
{
	bool is_changed = strcasecmp(old_state, new_state);
	bool is_start = !strcasecmp(new_state, "start");
	bool is_stop = !strcasecmp(new_state, "stop");

	/* The buffer where the whole configuration is stored.
	 * FIXME: use vmalloc() with the size of the configuration file. */
	static char cfg_text_buf[65536];

	TFW_LOG("got state via sysctl: %s\n", new_state);

	if (!is_changed) {
		TFW_LOG("the state '%s' isn't changed, nothing to do\n", new_state);
		return 0;
	}

	if (is_start) {
		int ret;

		TFW_DBG("reading configuration file...\n");
		ret = read_file_via_vfs(tfw_cfg_path, cfg_text_buf,
					sizeof(cfg_text_buf));


		TFW_LOG("starting all modules...\n");
		ret = tfw_cfg_start_mods(cfg_text_buf, &tfw_cfg_mods);
		if (ret)
			TFW_ERR("failed to start modules\n");
		return ret;

		tfw_cfg_mods_are_started = true;
	}

	if (is_stop) {
		TFW_LOG("stopping all modules...\n");
		tfw_cfg_stop_mods(&tfw_cfg_mods);
		tfw_cfg_mods_are_started = false;
		return 0;
	}

	/* Neither "start" or "stop"? */
	return -EINVAL;
}

/**
 * Syctl handler for tempesta.state read/write operations.
 */
static int
handle_sysctl_state_io(ctl_table *ctl, int is_write, void __user *user_buf,
		       size_t *lenp, loff_t *ppos)
{
	int r = 0;

	if (is_write) {
		char new_state_buf[ctl->maxlen];
		char *new_state, *old_state;
		size_t copied_data_len;

		copied_data_len = min((size_t)ctl->maxlen, *lenp);
		r = copy_from_user(new_state_buf, user_buf, copied_data_len);
		if (r)
			return r;

		new_state_buf[copied_data_len] = '\0';
		new_state = strim(new_state_buf);
		old_state = ctl->data;

		r = handle_state_change(old_state, new_state);
		if (r)
			return r;
	}

	r = proc_dostring(ctl, is_write, user_buf, lenp, ppos);

	return r;
}

static struct ctl_table_header *tfw_cfg_sysctl_hdr;

int
tfw_cfg_mod_if_init(void)
{
	static ctl_table tfw_cfg_sysctl_tbl[] = {
		{
			.procname	= "state",
			.data		= tfw_cfg_sysctl_state_buf,
			.maxlen		= sizeof(tfw_cfg_sysctl_state_buf) - 1,
			.mode		= 0644,
			.proc_handler	= handle_sysctl_state_io,
		},
		{}
	};

	tfw_cfg_sysctl_hdr = register_net_sysctl(&init_net, "net/tempesta",
						 tfw_cfg_sysctl_tbl);
	if (!tfw_cfg_sysctl_hdr) {
		TFW_ERR("can't register sysctl table\n");
		return -1;
	}

	return 0;
}

/**
 * The global shutdown routine: stop and un-register all modules,
 * and then un-register the sysctl interface.
 */
void
tfw_cfg_mod_if_exit(void)
{
	TfwCfgMod *mod, *tmp;

	TFW_DBG("stopping and unregistering all modules\n");

	if (tfw_cfg_mods_are_started)
		tfw_cfg_stop_mods(&tfw_cfg_mods);

	list_for_each_entry_safe_reverse(mod, tmp, &tfw_cfg_mods, list) {
		tfw_cfg_mod_unregister(mod);
	}
	unregister_net_sysctl_table(tfw_cfg_sysctl_hdr);
}

/**
 * Add @mod to the global list of registered modules and call @mod->init.
 *
 * After the registration the module starts receiving start/stop/setup/cleanup
 * events and configuration updates.
 */
int
tfw_cfg_mod_register(TfwCfgMod *mod)
{
	int ret;

	BUG_ON(!mod || !mod->name);

	TFW_LOG("register module: %s\n", mod->name);

	if (tfw_cfg_mods_are_started) {
		TFW_ERR("can't register module: %s - Tempesta FW is running\n",
			mod->name);
		return -EPERM;
	}

	ret = MOD_CALL_RET(mod, init);
	if (ret) {
		TFW_ERR("can't register module: %s - init callback returned "
			"error: %d\n", mod->name, ret);
		return ret;
	}

	INIT_LIST_HEAD(&mod->list);
	list_add_tail(&mod->list, &tfw_cfg_mods);

	return 0;
}
EXPORT_SYMBOL(tfw_cfg_mod_register);

/**
 * Remove the @mod from the global list and call the @mod->exit callback.
 */
void
tfw_cfg_mod_unregister(TfwCfgMod *mod)
{
	BUG_ON(!mod || !mod->name);

	/* We can't return an error code here because the function may be called
	 * from a module_exit() routine that shall not fail.
	 * Also we can't produce BUG() here because it may hang the system on
	 * forced module removal. */
	WARN(tfw_cfg_mods_are_started,
	     "Module '%s' is unregistered while Tempesta FW is running.\n"
	     "Other modules may still reference this unloaded module.\n"
	     "This is dangerous. Continuing with fingers crossed...\n",
	     mod->name);

	list_del(&mod->list);
	MOD_CALL(mod, exit);
}
EXPORT_SYMBOL(tfw_cfg_mod_unregister);