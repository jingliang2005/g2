/*
 * json_parser.cpp - JSON parser for TinyG
 * Part of TinyG2 project
 *
 * Copyright (c) 2011 - 2013 Alden S. Hart Jr.
 *
 * This file ("the software") is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2 as published by the
 * Free Software Foundation. You should have received a copy of the GNU General Public
 * License, version 2 along with the software.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, you may use this file as part of a software library without
 * restriction. Specifically, if other files instantiate templates or use macros or
 * inline functions from this file, or you compile this file and link it with  other
 * files to produce an executable, this file does not by itself cause the resulting
 * executable to be covered by the GNU General Public License. This exception does not
 * however invalidate any other reasons why the executable file might be covered by the
 * GNU General Public License.
 *
 * THE SOFTWARE IS DISTRIBUTED IN THE HOPE THAT IT WILL BE USEFUL, BUT WITHOUT ANY
 * WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
 * SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "tinyg2.h"
#include "controller.h"
#include "config.h"					// JSON sits on top of the config system
#include "json_parser.h"
#include "util.h"
#include "xio.h"					// for char definitions

#ifdef __cplusplus
extern "C"{
#endif

// local scope stuff

static stat_t _json_parser_kernal(char_t *str);
static stat_t _get_nv_pair_strict(cmdObj_t *cmd, char_t **pstr, int8_t *depth);
static stat_t _normalize_json_string(char_t *str, uint16_t size);
//static stat_t _gcode_comment_overrun_hack(cmdObj_t *cmd);

/****************************************************************************
 * json_parser() - exposed part of JSON parser
 * _json_parser_kernal()
 * _normalize_json_string()
 * _get_nv_pair_strict()
 *
 *	This is a dumbed down JSON parser to fit in limited memory with no malloc
 *	or practical way to do recursion ("depth" tracks parent/child levels).
 *
 *	This function will parse the following forms up to the JSON_MAX limits:
 *	  {"name":"value"}
 *	  {"name":12345}
 *	  {"name1":"value1", "n2":"v2", ... "nN":"vN"}
 *	  {"parent_name":""}
 *	  {"parent_name":{"name":"value"}}
 *	  {"parent_name":{"name1":"value1", "n2":"v2", ... "nN":"vN"}}
 *
 *	  "value" can be a string, number, true, false, or null (2 types)
 *
 *	Numbers
 *	  - number values are not quoted and can start with a digit or -. 
 *	  - numbers cannot start with + or . (period)
 *	  - exponentiated numbers are handled OK. 
 *	  - hexadecimal or other non-decimal number bases are not supported
 *
 *	The parser:
 *	  - extracts an array of one or more JSON object structs from the input string
 *	  - once the array is built it executes the object(s) in order in the array
 *	  - passes the executed array to the response handler to generate the response string
 *	  - returns the status and the JSON response string
 *
 *	Separation of concerns
 *	  js_json_parser() is the only exposed part. It does parsing, display, and status reports.
 *	  _get_nv_pair() only does parsing and syntax; no semantic validation or group handling
 *	  _json_parser_kernal() does index validation and group handling and executes sets and gets
 *		in an application agnostic way. It should work for other apps than TinyG 
 */

void json_parser(char_t *str)
{
	cmd_reset_list();				// get a fresh cmdObj list
	stat_t status = _json_parser_kernal(str);
	cmd_print_list(status, TEXT_NO_PRINT, JSON_RESPONSE_FORMAT);
//	rpt_request_status_report();	// generate an incremental status report if there are gcode model changes
}

static stat_t _json_parser_kernal(char_t *str)
{
	stat_t status;
	int8_t depth;
	cmdObj_t *cmd = cmd_body;
	char_t group[CMD_GROUP_LEN+1] = {""};		// group identifier - starts as NUL
	int8_t i = CMD_BODY_LEN;

	ritorno(_normalize_json_string(str, JSON_OUTPUT_STRING_MAX));	// return if error

	// parse the JSON command into the cmd body
	do {
		if (--i == 0) { return (STAT_JSON_TOO_MANY_PAIRS); }		// length error
		if ((status = _get_nv_pair_strict(cmd, &str, &depth)) > STAT_EAGAIN) {	// erred out
			return (status);
		}
		// propagate the group from previous NV pair (if relevant)
		if (group[0] != NUL) {
			strncpy(cmd->group, group, CMD_GROUP_LEN);// copy the parent's group to this child
		}
		// validate the token and get the index
		if ((cmd->index = cmd_get_index(cmd->group, cmd->token)) == NO_MATCH) { 
			return (STAT_UNRECOGNIZED_COMMAND);
		}
		if ((cmd_index_is_group(cmd->index)) && (cmd_group_is_prefixed(cmd->token))) {
			strncpy(group, cmd->token, CMD_GROUP_LEN);// record the group ID
		}
		cmd = cmd->nx;
	} while (status != STAT_OK);				// breaks when parsing is complete

	// execute the command
	cmd = cmd_body;
	if (cmd->type == TYPE_NULL){				// means GET the value
		ritorno(cmd_get(cmd));					// ritorno returns w/status on any errors
	} else {
		ritorno(cmd_set(cmd));					// set value or call a function (e.g. gcode)
		cmd_persist(cmd);
	}
	return (STAT_OK);							// only successful commands exit through this point
}

/*
 * _normalize_json_string - normalize a JSON string in place
 *
 *	Validate string size limits, remove all whitespace and convert 
 *	to lower case, with the exception of gcode comments
 */

static stat_t _normalize_json_string(char_t *str, uint16_t size)
{
	char_t *wr;								// write pointer
	uint8_t in_comment = false;

	if (strlen(str) > size) return (STAT_INPUT_EXCEEDS_MAX_LENGTH);

	for (wr = str; *str != NUL; str++) {
		if (!in_comment) {					// normal processing
			if (*str == '(') in_comment = true;
			if ((*str <= ' ') || (*str == DEL)) continue; // toss ctrls, WS & DEL
			*wr++ = tolower(*str);
		} else {							// Gcode comment processing	
			if (*str == ')') in_comment = false;
			*wr++ = *str;
		}
	}
	*wr = NUL;
	return (STAT_OK);
}

/*
 * _get_nv_pair_strict() - get the next name-value pair w/strict JSON rules
 *
 *	Parse the next statement and populate the command object (cmdObj).
 *
 *	Leaves string pointer (str) on the first character following the object.
 *	Which is the character just past the ',' separator if it's a multi-valued 
 *	object or the terminating NUL if single object or the last in a multi.
 *
 *	Keeps track of tree depth and closing braces as much as it has to.
 *	If this were to be extended to track multiple parents or more than two
 *	levels deep it would have to track closing curlies - which it does not.
 *
 *	ASSUMES INPUT STRING HAS FIRST BEEN NORMALIZED BY _normalize_json_string()
 *
 *	If a group prefix is passed in it will be pre-pended to any name parsed
 *	to form a token string. For example, if "x" is provided as a group and 
 *	"fr" is found in the name string the parser will search for "xfr"in the 
 *	cfgArray.
 */
static stat_t _get_nv_pair_strict(cmdObj_t *cmd, char_t **pstr, int8_t *depth)
{
	char_t *tmp;
	char_t terminators[] = {"},"};

	cmd_reset_obj(cmd);								// wipes the object and sets the depth

	// --- Process name part ---
	// find leading and trailing name quotes and set pointers.
	if ((*pstr = strchr(*pstr, '\"')) == NULL) { return (STAT_JSON_SYNTAX_ERROR);}
	if ((tmp = strchr(++(*pstr), '\"')) == NULL) { return (STAT_JSON_SYNTAX_ERROR);}
	*tmp = NUL;
	strncpy(cmd->token, *pstr, CMD_TOKEN_LEN);		// copy the string to the token

	// --- Process value part ---  (organized from most to least frequently encountered)
	*pstr = ++tmp;
	if ((*pstr = strchr(*pstr, ':')) == NULL) return (STAT_JSON_SYNTAX_ERROR);
	(*pstr)++;										// advance to start of value field

	// nulls (gets)
	if ((**pstr == 'n') || ((**pstr == '\"') && (*(*pstr+1) == '\"'))) { // process null value
		cmd->type = TYPE_NULL;
		cmd->value = TYPE_NULL;
	
	// numbers
	} else if (isdigit(**pstr) || (**pstr == '-')) {// value is a number
		cmd->value = strtod(*pstr, &tmp);			// tmp is the end pointer
		if(tmp == *pstr) { return (STAT_BAD_NUMBER_FORMAT);}
		cmd->type = TYPE_FLOAT;

	// object parent
	} else if (**pstr == '{') { 
		cmd->type = TYPE_PARENT;
//		*depth += 1;								// cmd_reset_obj() sets the next object's level so this is redundant
		(*pstr)++;
		return(STAT_EAGAIN);							// signal that there is more to parse

	// strings
	} else if (**pstr == '\"') { 					// value is a string
		(*pstr)++;
		cmd->type = TYPE_STRING;
		if ((tmp = strchr(*pstr, '\"')) == NULL) { return (STAT_JSON_SYNTAX_ERROR);} // find the end of the string
		*tmp = NUL;
		ritorno(cmd_copy_string(cmd, *pstr));
		*pstr = ++tmp;

	// boolean true/false
	} else if (**pstr == 't') { 
		cmd->type = TYPE_BOOL;
		cmd->value = true;
	} else if (**pstr == 'f') { 
		cmd->type = TYPE_BOOL;
		cmd->value = false;

	// arrays
	} else if (**pstr == '[') {
		cmd->type = TYPE_ARRAY;
		ritorno(cmd_copy_string(cmd, *pstr));		// copy array into string for error displays
		return (STAT_INPUT_VALUE_UNSUPPORTED);		// return error as the parser doesn't do input arrays yet

	// general error condition
	} else { return (STAT_JSON_SYNTAX_ERROR); }		// ill-formed JSON

	// process comma separators and end curlies
	if ((*pstr = strpbrk(*pstr, terminators)) == NULL) { // advance to terminator or err out
		return (STAT_JSON_SYNTAX_ERROR);
	}
	if (**pstr == '}') { 
		*depth -= 1;								// pop up a nesting level
		(*pstr)++;									// advance to comma or whatever follows
	}
	if (**pstr == ',') { return (STAT_EAGAIN);}		// signal that there is more to parse

	(*pstr)++;
	return (STAT_OK);								// signal that parsing is complete
}

/****************************************************************************
 * json_serialize() - make a JSON object string from JSON object array
 *
 *	*cmd is a pointer to the first element in the cmd list to serialize
 *	*out_buf is a pointer to the output string - usually what was the input string
 *	Returns the character count of the resulting string
 *
 * 	Operation:
 *	  - The cmdObj list is processed start to finish with no recursion
 *	  - Assume the first object is depth 0 or greater (the opening curly)
 *	  - Assume remaining depths have been set correctly; but might not achieve closure;
 *		e.g. list starts on 0, and ends on 3, in which case provide correct closing curlies
 *	  - Assume there can be multiple, independent, non-contiguous JSON objects at a 
 *		given depth value. These are processed correctly - e.g. 0,1,1,0,1,1,0,1,1
 *	  - The list must have a terminating cmdObj where cmd->nx == NULL. 
 *		The terminating object may or may not have data (empty or not empty).
 *
 *	Desired behaviors:
 *	  - Allow self-referential elements that would otherwise cause a recursive loop
 *	  - Skip over empty objects (TYPE_EMPTY)
 *	  - If a JSON object is empty represent it as {}
 *	    --- OR ---
 *	  - If a JSON object is empty omit the object altogether (no curlies)
 *
 *	Note: TYPE_FLOAT_UNITS is used to convert a value back to inches mode for display
 *		  that was previously converted to MM mode for internal operations.
 */

#define BUFFER_MARGIN 8			// safety margin to avoid buffer overruns

uint16_t json_serialize(cmdObj_t *cmd, char_t *out_buf, uint16_t size)
{
	char_t *str = out_buf;
	char_t *str_max = out_buf + size - BUFFER_MARGIN;
	int8_t initial_depth = cmd->depth;
	int8_t prev_depth = 0;
	uint8_t need_a_comma = false;

	*str++ = '{'; 								// write opening curly

	while (true) {
		if (cmd->type != TYPE_EMPTY) {
			if (need_a_comma) { *str++ = ',';}
			need_a_comma = true;
			str += sprintf((char *)str, (char *)"\"%s\":", (char *)cmd->token);

//			if (cmd->type == TYPE_FLOAT_UNITS)	{ 
//				if (cm_get_units_mode() == INCHES) { cmd->value /= MM_PER_INCH;}
//				cmd->type = TYPE_FLOAT;
//			}
			if (cmd->type == TYPE_NULL)	{ str += sprintf((char *)str, (char *)"\"\"");}
//			if (cmd->type == TYPE_NULL)	{ str += sprintf(str, "\"\"");}
			else if (cmd->type == TYPE_INTEGER)	{ str += sprintf((char *)str, "%1.0f", cmd->value);}
			else if (cmd->type == TYPE_STRING)	{ str += sprintf((char *)str, "\"%s\"",(char *)*cmd->stringp);}
			else if (cmd->type == TYPE_ARRAY)	{ str += sprintf((char *)str, "[%s]",  (char *)*cmd->stringp);}
			else if (cmd->type == TYPE_FLOAT) {
				if 		(cmd->precision == 0) { str += sprintf((char *)str, "%0.0f", cmd->value);}
				else if (cmd->precision == 1) { str += sprintf((char *)str, "%0.1f", cmd->value);}
				else if (cmd->precision == 2) { str += sprintf((char *)str, "%0.2f", cmd->value);}
				else if (cmd->precision == 3) { str += sprintf((char *)str, "%0.3f", cmd->value);}
				else if (cmd->precision == 4) { str += sprintf((char *)str, "%0.4f", cmd->value);}
				else 						  { str += sprintf((char *)str, "%f", cmd->value);}
			}
			else if (cmd->type == TYPE_BOOL) {
				if (fp_FALSE(cmd->value)) { str += sprintf((char *)str, "false");}
				else { str += sprintf((char *)str, "true"); }
			}
			if (cmd->type == TYPE_PARENT) { 
				*str++ = '{';
				need_a_comma = false;
			}
		}
		if (str >= str_max) { return (-1);}		// signal buffer overrun
		if ((cmd = cmd->nx) == NULL) { break;}	// end of the list
		if (cmd->depth < prev_depth) {
			need_a_comma = true;
			*str++ = '}';						// and close the level
		}
		prev_depth = cmd->depth;
	}

	// closing curlies and NEWLINE
	while (prev_depth-- > initial_depth) { *str++ = '}';}
	str += sprintf((char *)str, "}\n");	// using sprintf for this last one ensures a NUL termination
	if (str > out_buf + size) { return (-1);}
	return (str - out_buf);
}

/*
uint16_t json_serialize(cmdObj_t *cmd, char_t *out_buf)
{
	char_t *str = out_buf;
	int8_t initial_depth = cmd->depth;
	int8_t prev_depth = 0;
	uint8_t need_a_comma = false;

	*str++ = '{'; 								// write opening curly
	while (true) {
		if (cmd->type != TYPE_EMPTY) {
			if (need_a_comma) { *str++ = ',';}
			need_a_comma = true;
			str += sprintf((char *)str, (char *)"\"%s\":", (char *)cmd->token);
			if (cmd->type == TYPE_NULL)	{ str += sprintf((char *)str, (char *)"\"\"");}
			else if (cmd->type == TYPE_INTEGER)	{ str += sprintf((char *)str, (char *)"%1.0f", cmd->value);}
			else if (cmd->type == TYPE_FLOAT)	{ str += sprintf((char *)str, (char *)"%0.3f", cmd->value);}
			else if (cmd->type == TYPE_STRING)	{ str += sprintf((char *)str, (char *)"\"%s\"",*cmd->stringp);}
			else if (cmd->type == TYPE_ARRAY)	{ str += sprintf((char *)str, (char *)"[%s]",  *cmd->stringp);}
			else if (cmd->type == TYPE_BOOL) 	{
//				if (cmd->value == false) { str += sprintf((char *)str, "false");}
				if (fp_FALSE(cmd->value)) { str += sprintf((char *)str, "false");}
				else { str += sprintf((char *)str, "true"); }
			}
			if (cmd->type == TYPE_PARENT) { 
				*str++ = '{';
				need_a_comma = false;
			}
		}
		if ((cmd = cmd->nx) == NULL) { break;}	// end of the list
		if (cmd->depth < prev_depth) {
			need_a_comma = true;
			*str++ = '}';						// and close the level
		}
		prev_depth = cmd->depth;
	}
	// closing curlies and NEWLINE
	while (prev_depth-- > initial_depth) { *str++ = '}';}
	str += sprintf((char *)str, (char *)"}\n");	// using sprintf for this last one ensures a NUL termination
	return (str - out_buf);
}
*/

/*
 * json_print_object() - serialize and print the cmdObj array directly (w/o header & footer)
 *
 *	Ignores JSON verbosity settings and everything else - just serializes the list & prints
 *	Useful for reports and other simple output.
 *	Object list should be terminated by cmd->nx == NULL
 */
void json_print_object(cmdObj_t *cmd)
{
	json_serialize(cmd, cs.out_buf, sizeof(cs.out_buf));
	fprintf(stderr, "%s", cs.out_buf);
}

/*
 * json_print_response() - JSON responses with headers, footers and observes JSON verbosity 
 *
 *	A footer is returned for every setting except $jv=0
 *
 *	JV_SILENT = 0,	// no response is provided for any command
 *	JV_FOOTER,		// responses contain  footer only; no command echo, gcode blocks or messages
 *	JV_CONFIGS,		// echo configs; gcode blocks are not echoed; messages are not echoed
 *	JV_MESSAGES,	// echo configs; gcode messages only (if present); no block echo or line numbers
 *	JV_LINENUM,		// echo configs; gcode blocks return messages and line numbers as present
 *	JV_VERBOSE		// echos all configs and gcode blocks, line numbers and messages
 *
 *	This gets a bit complicated. The first cmdObj is the header, which must be set by reset_list().
 *	The first object in the body will always have the gcode block or config command in it, 
 *	which you may or may not want to display. This is followed by zero or more displayable objects. 
 *	Then if you want a gcode line number you add that here to the end. Finally, a footer goes 
 *	on all the (non-silent) responses.
 */
#define MAX_TAIL_LEN 8

void json_print_response(stat_t status)
{
	json_print_object(cmd_list);
}

//###########################################################################
//##### UNIT TESTS ##########################################################
//###########################################################################

#if defined __UNIT_TEST_JSON

void _test_parser(void);
void _test_serialize(void);
cmdObj_t * _reset_array(void);
cmdObj_t * _add_parent(cmdObj_t *cmd, char_t *token);
cmdObj_t * _add_string(cmdObj_t *cmd, char_t *token, char_t *string);
cmdObj_t * _add_integer(cmdObj_t *cmd, char_t *token, uint32_t integer);
cmdObj_t * _add_empty(cmdObj_t *cmd);
cmdObj_t * _add_array(cmdObj_t *cmd, char_t *footer);
char_t * _clr(char_t *buf);
void _printit(void);

#define ARRAY_LEN 8
	cmdObj_t cmd_array[ARRAY_LEN];

void js_unit_tests()
{
//	_test_parser();
	_test_serialize();
}

void _test_serialize()
{
	cmdObj_t *cmd = cmd_array;
//	printf("\n\nJSON serialization tests\n");

	// null list
	_reset_array();
	js_serialize_json(cmd_array, kc.out_buf);
	_printit();

	// parent with a null child
	cmd = _reset_array();
	cmd = _add_parent(cmd, "r");
	js_serialize_json(cmd_array, kc.out_buf);
	_printit();

	// single string element (message)
	cmd = _reset_array();
	cmd = _add_string(cmd, "msg", "test message");
	js_serialize_json(cmd_array, kc.out_buf);
	_printit();

	// string element and an integer element
	cmd = _reset_array();
	cmd = _add_string(cmd, "msg", "test message");
	cmd = _add_integer(cmd, "answer", 42);
	js_serialize_json(cmd_array, kc.out_buf);
	_printit();

	// parent with a string and an integer element
	cmd = _reset_array();
	cmd = _add_parent(cmd, "r");
	cmd = _add_string(cmd, "msg", "test message");
	cmd = _add_integer(cmd, "answer", 42);
	js_serialize_json(cmd_array, kc.out_buf);
	_printit();

	// parent with a null child followed by a final level 0 element (footer)
	cmd = _reset_array();
	cmd = _add_parent(cmd, "r");
	cmd = _add_empty(cmd);
	cmd = _add_string(cmd, "f", "[1,0,12,1234]");	// fake out a footer
	cmd->pv->depth = 0;
	js_serialize_json(cmd_array, kc.out_buf);
	_printit();

	// parent with a single element child followed by empties folowed by a final level 0 element
	cmd = _reset_array();
	cmd = _add_parent(cmd, "r");
	cmd = _add_integer(cmd, "answer", 42);
	cmd = _add_empty(cmd);
	cmd = _add_empty(cmd);
	cmd = _add_string(cmd, "f", "[1,0,12,1234]");	// fake out a footer
	cmd->pv->depth = 0;
	js_serialize_json(cmd_array, kc.out_buf);
	_printit();

	// response object parent with no children w/footer
	cmd_reset_list();								// works with the header/body/footer list
	_add_array(cmd, "1,0,12,1234");					// fake out a footer
	js_serialize_json(cmd_header, kc.out_buf);
	_printit();

	// response parent with one element w/footer
	cmd_reset_list();								// works with the header/body/footer list
	cmd_add_string("msg", "test message");
	_add_array(cmd, "1,0,12,1234");					// fake out a footer
	js_serialize_json(cmd_header, kc.out_buf);
	_printit();
}

static char_t * _clr(char_t *buf)
{
	for (uint8_t i=0; i<250; i++) {
		buf[i] = 0;
	}
	return (buf);
}

static void _printit(void)
{
//	printf("%s", kc.out_buf);	
}

static cmdObj_t * _reset_array()
{
	cmdObj_t *cmd = cmd_array;
	for (uint8_t i=0; i<ARRAY_LEN; i++) {
		if (i == 0) { cmd->pv = NULL; } 
		else { cmd->pv = (cmd-1);}
		cmd->nx = (cmd+1);
		cmd->index = 0;
		cmd->token[0] = NUL;
		cmd->depth = 0;
		cmd->type = TYPE_EMPTY;
		cmd++;
	}
	(--cmd)->nx = NULL;				// correct last element
	return (cmd_array);
}

static cmdObj_t * _add_parent(cmdObj_t *cmd, char_t *token)
{
	strncpy(cmd->token, token, CMD_TOKEN_LEN);
	cmd->nx->depth = cmd->depth+1;
	cmd->type = TYPE_PARENT;
	return (cmd->nx);
}

static cmdObj_t * _add_string(cmdObj_t *cmd, char_t *token, char_t *string)
{
	strncpy(cmd->token, token, CMD_TOKEN_LEN);
	cmd_copy_string(cmd, string);
	if (cmd->depth < cmd->pv->depth) { cmd->depth = cmd->pv->depth;}
	cmd->type = TYPE_STRING;
	return (cmd->nx);
}

static cmdObj_t * _add_integer(cmdObj_t *cmd, char_t *token, uint32_t integer)
{
	strncpy(cmd->token, token, CMD_TOKEN_LEN);
	cmd->value = (float)integer;
	if (cmd->depth < cmd->pv->depth) { cmd->depth = cmd->pv->depth;}
	cmd->type = TYPE_INTEGER;
	return (cmd->nx);
}

static cmdObj_t * _add_empty(cmdObj_t *cmd)
{
	if (cmd->depth < cmd->pv->depth) { cmd->depth = cmd->pv->depth;}
	cmd->type = TYPE_EMPTY;
	return (cmd->nx);
}

static cmdObj_t * _add_array(cmdObj_t *cmd, char_t *array_string)
{
	cmd->type = TYPE_ARRAY;
//	strncpy(cmd->string, array_string, CMD_STRING_LEN);
	cmd_copy_string(cmd, array_string);
	return (cmd->nx);
}

static void _test_parser()
{
// tip: breakpoint the js_json_parser return (STAT_OK) and examine the js[] array

// success cases

	// single NV pair cases
	js_json_parser("{\"config_version\":null}\n");					// simple null test
	js_json_parser("{\"config_profile\":true}\n");					// simple true test
	js_json_parser("{\"prompt\":false}\n");							// simple false test
	js_json_parser("{\"gcode\":\"g0 x3 y4 z5.5 (comment line)\"}\n");// string test w/comment
	js_json_parser("{\"x_feedrate\":1200}\n");						// numeric test
	js_json_parser("{\"y_feedrate\":-1456}\n");						// numeric test

	js_json_parser("{\"Z_velocity_maximum\":null}\n");				// axis w/null
	js_json_parser("{\"m1_microsteps\":null}\n");					// motor w/null
	js_json_parser("{\"2mi\":8}\n");								// motor token w/null
	js_json_parser("{\"no-token\":12345}\n");						// non-token w/number

	// multi-pair cases					 tabs here V
	js_json_parser("{\"firmware_version\":329.26,		\"config_version\":0.93}\n");
	js_json_parser("{\"1mi\":8, \"2mi\":8,\"3mi\":8,\"4mi\":8}\n");	// 4 elements

	// parent / child cases
	js_json_parser("{\"status_report\":{\"ln\":true, \"x_pos\":true, \"y_pos\":true, \"z_pos\":true}}\n");
	js_json_parser("{\"parent_case1\":{\"child_null\":null}}\n");	// parent w/single child
	js_json_parser("{\"parent_case2\":{\"child_num\":23456}}\n");	// parent w/single child
	js_json_parser("{\"parent_case3\":{\"child_str\":\"stringdata\"}}\n");// parent w/single child

// error cases

	js_json_parser("{\"err_1\":36000x\n}");							// illegal number 
	js_json_parser("{\"err_2\":\"text\n}");							// no string termination
	js_json_parser("{\"err_3\":\"12345\",}\n");						// bad } termination
	js_json_parser("{\"err_4\":\"12345\"\n");						// no } termination

}

#endif // __UNIT_TEST_JSON

#ifdef __cplusplus
}
#endif // __cplusplus
