/*
 * dcl_exec.c - DCL Command Execution Dispatch
 *
 * Dispatches parsed DCL commands to their handler functions.
 * Handles symbol assignment, procedure invocation, IF/THEN/ELSE
 * flow control, and command verb lookup with minimum-uniqueness.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "dcl/context.h"
#include "dcl/parser.h"
#include "dcl/symbol.h"
#include "dcl/cdu.h"
#include "ssdef.h"

/* External functions */
extern int dcl_execute_script(const char *filename, int argc, char **argv);
extern void dcl_error(const char *facility, int severity, const char *ident,
                      const char *fmt, ...);
extern int dcl_eval_lexical(struct dcl_context *ctx, const char *expr,
                            char *result, size_t result_size);
extern int dcl_find_label(FILE *fp, const char *label);

/*
 * Evaluate a simple DCL expression for IF conditions.
 * Returns 1 if true, 0 if false.
 *
 * Supports:
 *   expr .EQS. expr  (string comparison)
 *   expr .NES. expr
 *   expr .EQ. expr   (integer comparison)
 *   expr .NE. expr
 *   expr .LT. expr, .GT., .LE., .GE.
 *   expr .LTS. expr, .GTS., .LES., .GES.
 */
static int eval_condition(struct dcl_context *ctx, const char *expr)
{
    if (!expr) return 0;

    /* Find the operator */
    static const struct {
        const char *op;
        int is_string;
        int type;  /* 0=eq, 1=ne, 2=lt, 3=gt, 4=le, 5=ge */
    } ops[] = {
        { ".EQS.", 1, 0 }, { ".NES.", 1, 1 },
        { ".LTS.", 1, 2 }, { ".GTS.", 1, 3 },
        { ".LES.", 1, 4 }, { ".GES.", 1, 5 },
        { ".EQ.",  0, 0 }, { ".NE.",  0, 1 },
        { ".LT.",  0, 2 }, { ".GT.",  0, 3 },
        { ".LE.",  0, 4 }, { ".GE.",  0, 5 },
    };

    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        /* Case-insensitive search for operator */
        const char *p = expr;
        while (*p) {
            size_t oplen = strlen(ops[i].op);
            int match = 1;
            for (size_t j = 0; j < oplen && p[j]; j++) {
                if (toupper((unsigned char)p[j]) != ops[i].op[j]) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                /* Extract left and right operands */
                char left[1024] = {0};
                char right[1024] = {0};

                size_t llen = (size_t)(p - expr);
                if (llen >= sizeof(left)) llen = sizeof(left) - 1;
                memcpy(left, expr, llen);
                left[llen] = '\0';

                const char *r = p + strlen(ops[i].op);
                strncpy(right, r, sizeof(right) - 1);
                right[sizeof(right) - 1] = '\0';

                /* Trim whitespace */
                char *ls = left;
                while (*ls == ' ' || *ls == '\t') ls++;
                size_t ll = strlen(ls);
                while (ll > 0 && (ls[ll - 1] == ' ' || ls[ll - 1] == '\t'))
                    ls[--ll] = '\0';

                char *rs = right;
                while (*rs == ' ' || *rs == '\t') rs++;
                size_t rl = strlen(rs);
                while (rl > 0 && (rs[rl - 1] == ' ' || rs[rl - 1] == '\t'))
                    rs[--rl] = '\0';

                /* Unquote strings */
                if (ll >= 2 && ls[0] == '"' && ls[ll - 1] == '"') {
                    ls[ll - 1] = '\0'; ls++;
                }
                if (rl >= 2 && rs[0] == '"' && rs[rl - 1] == '"') {
                    rs[rl - 1] = '\0'; rs++;
                }

                /* Evaluate lexical functions if present */
                char lval[1024], rval[1024];
                if (strncasecmp(ls, "F$", 2) == 0) {
                    dcl_eval_lexical(ctx, ls, lval, sizeof(lval));
                    ls = lval;
                } else {
                    strncpy(lval, ls, sizeof(lval) - 1);
                    ls = lval;
                }
                if (strncasecmp(rs, "F$", 2) == 0) {
                    dcl_eval_lexical(ctx, rs, rval, sizeof(rval));
                    rs = rval;
                } else {
                    strncpy(rval, rs, sizeof(rval) - 1);
                    rs = rval;
                }

                int result;
                if (ops[i].is_string) {
                    int cmp = strcmp(ls, rs);
                    switch (ops[i].type) {
                        case 0: result = (cmp == 0); break;
                        case 1: result = (cmp != 0); break;
                        case 2: result = (cmp < 0); break;
                        case 3: result = (cmp > 0); break;
                        case 4: result = (cmp <= 0); break;
                        case 5: result = (cmp >= 0); break;
                        default: result = 0;
                    }
                } else {
                    long lv = strtol(ls, NULL, 0);
                    long rv = strtol(rs, NULL, 0);
                    switch (ops[i].type) {
                        case 0: result = (lv == rv); break;
                        case 1: result = (lv != rv); break;
                        case 2: result = (lv < rv); break;
                        case 3: result = (lv > rv); break;
                        case 4: result = (lv <= rv); break;
                        case 5: result = (lv >= rv); break;
                        default: result = 0;
                    }
                }
                return result;
            }
            p++;
        }
    }

    /* No operator found - treat as boolean:
     * non-empty non-zero string = true
     * "TRUE" = true, "FALSE" = false
     * 0 = false, non-zero = true */
    const char *p = expr;
    while (*p == ' ' || *p == '\t') p++;
    char val[256];
    strncpy(val, p, sizeof(val) - 1);
    val[sizeof(val) - 1] = '\0';
    size_t vlen = strlen(val);
    while (vlen > 0 && (val[vlen - 1] == ' ' || val[vlen - 1] == '\t'))
        val[--vlen] = '\0';

    if (strcasecmp(val, "TRUE") == 0 || strcasecmp(val, "1") == 0) return 1;
    if (strcasecmp(val, "FALSE") == 0 || strcasecmp(val, "0") == 0 ||
        val[0] == '\0') return 0;

    /* Try numeric */
    long v = strtol(val, NULL, 0);
    return (v != 0) ? 1 : 0;
}

/*
 * Execute a symbol assignment.
 */
static int exec_assign(struct dcl_context *ctx, struct dcl_command *cmd)
{
    (void)ctx;

    int scope = DCL_SYM_LOCAL;
    const char *value = cmd->rest;

    /* Determine assignment type from label field */
    if (strcmp(cmd->label, "==") == 0 || strcmp(cmd->label, ":==") == 0) {
        scope = DCL_SYM_GLOBAL;
    }

    /* For := and :==, the value is a string (upcase and trim) */
    if (cmd->label[0] == ':') {
        char trimmed[DCL_MAX_VALUE];
        const char *v = value;
        while (*v == ' ' || *v == '\t') v++;
        strncpy(trimmed, v, sizeof(trimmed) - 1);
        trimmed[sizeof(trimmed) - 1] = '\0';
        size_t len = strlen(trimmed);
        while (len > 0 && (trimmed[len - 1] == ' ' || trimmed[len - 1] == '\t'))
            trimmed[--len] = '\0';

        /* For := string assignment, upcase and compress spaces */
        char result[DCL_MAX_VALUE];
        size_t ri = 0;
        int in_space = 0;
        int in_quote = 0;
        for (size_t i = 0; trimmed[i] && ri < sizeof(result) - 1; i++) {
            if (trimmed[i] == '"') {
                in_quote = !in_quote;
                continue; /* Don't include quotes in result */
            }
            if (!in_quote && (trimmed[i] == ' ' || trimmed[i] == '\t')) {
                if (!in_space) {
                    result[ri++] = ' ';
                    in_space = 1;
                }
            } else {
                if (in_quote) {
                    result[ri++] = trimmed[i];
                } else {
                    result[ri++] = (char)toupper((unsigned char)trimmed[i]);
                }
                in_space = 0;
            }
        }
        result[ri] = '\0';

        dcl_sym_set(cmd->verb, result, scope);
    } else {
        /* Regular = or == assignment */
        char trimmed[DCL_MAX_VALUE];
        const char *v = value;
        while (*v == ' ' || *v == '\t') v++;
        strncpy(trimmed, v, sizeof(trimmed) - 1);
        trimmed[sizeof(trimmed) - 1] = '\0';
        size_t len = strlen(trimmed);
        while (len > 0 && (trimmed[len - 1] == ' ' || trimmed[len - 1] == '\t'))
            trimmed[--len] = '\0';

        /* Remove surrounding quotes from string values */
        if (len >= 2 && trimmed[0] == '"' && trimmed[len - 1] == '"') {
            trimmed[len - 1] = '\0';
            memmove(trimmed, trimmed + 1, len - 1);
        }

        /* Check if it's a lexical function */
        if (strncasecmp(trimmed, "F$", 2) == 0) {
            char result[DCL_MAX_VALUE];
            dcl_eval_lexical(ctx, trimmed, result, sizeof(result));
            dcl_sym_set(cmd->verb, result, scope);
        } else {
            /* Check for arithmetic expression */
            /* For now, handle simple integer expressions */
            char *endp;
            long val = strtol(trimmed, &endp, 0);
            if (*endp == '\0' && trimmed[0] != '\0') {
                /* Pure integer */
                dcl_sym_set_int(cmd->verb, (int32_t)val, scope);
            } else {
                dcl_sym_set(cmd->verb, trimmed, scope);
            }
        }
    }

    return SS$_NORMAL;
}

/*
 * Execute a parsed DCL command line.
 * This is the main dispatch function.
 */
int dcl_execute_command(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (!cmd) return SS$_BADPARAM;

    /* Check if we're in a skipped IF block */
    if (ctx->if_depth > 0 && ctx->if_stack[ctx->if_depth - 1].skip) {
        /* Only process ELSE, ENDIF, and IF (for nesting) */
        if (strcasecmp(cmd->verb, "ELSE") != 0 &&
            strcasecmp(cmd->verb, "ENDIF") != 0 &&
            strcasecmp(cmd->verb, "IF") != 0) {
            return SS$_NORMAL;
        }
    }

    /* Handle different command types */
    switch (cmd->type) {
    case DCL_NODE_ASSIGN:
        return exec_assign(ctx, cmd);

    case DCL_NODE_COMMENT:
    case DCL_NODE_LABEL:
        return SS$_NORMAL;

    case DCL_NODE_COMMAND:
        break; /* Fall through to verb dispatch */

    default:
        break;
    }

    /* Handle special verbs that aren't in the command table */
    if (strcasecmp(cmd->verb, "IF") == 0) {
        /* Parse: IF condition THEN command */
        /* The condition and THEN clause are in the params */
        /* Reconstruct the condition from params */
        char condition[DCL_MAX_LINE] = {0};
        char then_cmd[DCL_MAX_LINE] = {0};
        int found_then = 0;

        for (int i = 0; i < cmd->param_count; i++) {
            if (strcasecmp(cmd->params[i], "THEN") == 0) {
                found_then = 1;
                /* Collect the rest as the THEN command */
                for (int j = i + 1; j < cmd->param_count; j++) {
                    if (then_cmd[0]) strncat(then_cmd, " ",
                                             sizeof(then_cmd) - strlen(then_cmd) - 1);
                    strncat(then_cmd, cmd->params[j],
                            sizeof(then_cmd) - strlen(then_cmd) - 1);
                }
                break;
            } else {
                if (condition[0]) strncat(condition, " ",
                                          sizeof(condition) - strlen(condition) - 1);
                strncat(condition, cmd->params[i],
                        sizeof(condition) - strlen(condition) - 1);
            }
        }

        int cond_result = eval_condition(ctx, condition);

        if (found_then && then_cmd[0]) {
            /* Single-line IF: IF cond THEN cmd */
            if (cond_result) {
                return dcl_execute_line(then_cmd);
            }
            return SS$_NORMAL;
        } else {
            /* Multi-line IF block */
            if (ctx->if_depth >= DCL_MAX_NEST) {
                dcl_error("DCL", 4, "NESTLEV",
                          "maximum IF nesting level exceeded");
                return SS$_BADPARAM;
            }
            ctx->if_stack[ctx->if_depth].in_if = 1;
            ctx->if_stack[ctx->if_depth].condition_true = cond_result;
            ctx->if_stack[ctx->if_depth].in_else = 0;
            ctx->if_stack[ctx->if_depth].skip = !cond_result;
            ctx->if_depth++;
            return SS$_NORMAL;
        }
    }

    if (strcasecmp(cmd->verb, "ELSE") == 0) {
        if (ctx->if_depth <= 0) {
            dcl_error("DCL", 2, "NOIFBLK", "ELSE without IF");
            return SS$_BADPARAM;
        }
        ctx->if_stack[ctx->if_depth - 1].in_else = 1;
        ctx->if_stack[ctx->if_depth - 1].skip =
            ctx->if_stack[ctx->if_depth - 1].condition_true;
        return SS$_NORMAL;
    }

    if (strcasecmp(cmd->verb, "ENDIF") == 0) {
        if (ctx->if_depth <= 0) {
            dcl_error("DCL", 2, "NOIFBLK", "ENDIF without IF");
            return SS$_BADPARAM;
        }
        ctx->if_depth--;
        return SS$_NORMAL;
    }

    if (strcasecmp(cmd->verb, "GOTO") == 0) {
        if (cmd->param_count < 1) {
            dcl_error("DCL", 2, "NOLAB", "no label specified in GOTO");
            return SS$_BADPARAM;
        }
        if (ctx->proc_depth < 0) {
            dcl_error("DCL", 2, "NOINTERACT",
                      "GOTO not allowed in interactive mode");
            return SS$_BADPARAM;
        }
        FILE *fp = ctx->proc_stack[ctx->proc_depth].fp;
        if (dcl_find_label(fp, cmd->params[0]) != 0) {
            dcl_error("DCL", 2, "USGOTO",
                      "target of GOTO not found - \\%s\\", cmd->params[0]);
            return SS$_BADPARAM;
        }
        return SS$_NORMAL;
    }

    if (strcasecmp(cmd->verb, "GOSUB") == 0) {
        if (cmd->param_count < 1) {
            dcl_error("DCL", 2, "NOLAB", "no label specified in GOSUB");
            return SS$_BADPARAM;
        }
        if (ctx->proc_depth < 0) {
            dcl_error("DCL", 2, "NOINTERACT",
                      "GOSUB not allowed in interactive mode");
            return SS$_BADPARAM;
        }
        if (ctx->gosub_depth >= DCL_MAX_NEST) {
            dcl_error("DCL", 4, "NESTLEV",
                      "maximum GOSUB nesting level exceeded");
            return SS$_BADPARAM;
        }
        /* Save return position */
        FILE *fp = ctx->proc_stack[ctx->proc_depth].fp;
        ctx->gosub_stack[ctx->gosub_depth].file_offset = ftell(fp);
        ctx->gosub_stack[ctx->gosub_depth].line_number =
            ctx->proc_stack[ctx->proc_depth].line_number;
        ctx->gosub_depth++;

        if (dcl_find_label(fp, cmd->params[0]) != 0) {
            ctx->gosub_depth--;
            dcl_error("DCL", 2, "USGOTO",
                      "target of GOSUB not found - \\%s\\", cmd->params[0]);
            return SS$_BADPARAM;
        }
        return SS$_NORMAL;
    }

    if (strcasecmp(cmd->verb, "RETURN") == 0) {
        if (ctx->gosub_depth <= 0) {
            dcl_error("DCL", 2, "NOGOSUB", "RETURN without GOSUB");
            return SS$_BADPARAM;
        }
        ctx->gosub_depth--;
        FILE *fp = ctx->proc_stack[ctx->proc_depth].fp;
        fseek(fp, ctx->gosub_stack[ctx->gosub_depth].file_offset, SEEK_SET);
        ctx->proc_stack[ctx->proc_depth].line_number =
            ctx->gosub_stack[ctx->gosub_depth].line_number;
        return SS$_NORMAL;
    }

    if (strcasecmp(cmd->verb, "ON") == 0) {
        /* ON ERROR THEN GOTO label / ON ERROR THEN CONTINUE */
        /* ON SEVERE_ERROR THEN GOTO label / ON SEVERE_ERROR THEN CONTINUE */
        if (cmd->param_count < 3) {
            dcl_error("DCL", 2, "IVKEYW", "invalid ON command syntax");
            return SS$_BADPARAM;
        }

        int is_severe = (strcasecmp(cmd->params[0], "SEVERE_ERROR") == 0);
        /* params[0] = ERROR/SEVERE_ERROR, params[1] = THEN, params[2] = CONTINUE/GOTO */

        if (strcasecmp(cmd->params[2], "CONTINUE") == 0) {
            if (ctx->proc_depth >= 0) {
                if (is_severe)
                    ctx->proc_stack[ctx->proc_depth].on_severe = 1;
                else
                    ctx->proc_stack[ctx->proc_depth].on_error = 1;
            } else {
                ctx->on_error_continue = 1;
            }
        } else if (strcasecmp(cmd->params[2], "GOTO") == 0) {
            if (cmd->param_count >= 4) {
                if (ctx->proc_depth >= 0) {
                    if (is_severe) {
                        ctx->proc_stack[ctx->proc_depth].on_severe = 2;
                        strncpy(ctx->proc_stack[ctx->proc_depth].on_severe_label,
                                cmd->params[3],
                                sizeof(ctx->proc_stack[0].on_severe_label) - 1);
                    } else {
                        ctx->proc_stack[ctx->proc_depth].on_error = 2;
                        strncpy(ctx->proc_stack[ctx->proc_depth].on_error_label,
                                cmd->params[3],
                                sizeof(ctx->proc_stack[0].on_error_label) - 1);
                    }
                }
            }
        }
        return SS$_NORMAL;
    }

    /* @ procedure */
    if (strcmp(cmd->verb, "@") == 0) {
        if (cmd->param_count < 1) {
            dcl_error("DCL", 2, "IVVERB", "missing procedure name");
            return SS$_BADPARAM;
        }
        /* Pass P1-P8 */
        char *params[8] = {NULL};
        int pcount = cmd->param_count - 1;
        for (int i = 0; i < pcount && i < 8; i++) {
            params[i] = cmd->params[i + 1];
        }
        return dcl_execute_script(cmd->params[0], pcount, params);
    }

    /* Look up the verb in the command table */
    const struct dcl_verb *verb = dcl_find_verb(cmd->verb);
    if (verb) {
        int status = verb->handler(cmd);
        ctx->last_status = (uint32_t)status;
        ctx->last_severity = (uint32_t)(status & 7);

        /* Update $STATUS and $SEVERITY symbols */
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", status);
        dcl_sym_set("$STATUS", buf, DCL_SYM_GLOBAL);
        snprintf(buf, sizeof(buf), "%d", status & 7);
        dcl_sym_set("$SEVERITY", buf, DCL_SYM_GLOBAL);

        return status;
    }

    /* Command not found */
    dcl_error("DCL", 2, "IVVERB",
              "unrecognized command verb - check validity and spelling\n"
              " \\%s\\", cmd->verb);
    return SS$_IVVERB;
}

/*
 * Execute a raw DCL command line string.
 * Performs parsing, then dispatches.
 */
int dcl_execute_line(const char *line)
{
    if (!line) return SS$_BADPARAM;

    /* Skip empty/whitespace lines */
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0' || *p == '!') return SS$_NORMAL;

    struct dcl_command cmd;
    if (dcl_parse_line(p, &cmd) != 0) {
        dcl_error("DCL", 2, "SYNTAX", "syntax error in command line");
        return SS$_BADPARAM;
    }

    if (cmd.type == DCL_NODE_COMMENT) return SS$_NORMAL;
    if (cmd.verb[0] == '\0' && cmd.label[0] != '\0') return SS$_NORMAL;
    if (cmd.verb[0] == '\0') return SS$_NORMAL;

    return dcl_execute_command(&cmd);
}
