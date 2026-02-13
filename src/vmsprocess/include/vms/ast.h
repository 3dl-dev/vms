#ifndef __VMS_AST_H
#define __VMS_AST_H

#include <stdint.h>

/* AST (Asynchronous System Trap) - VMS async callback mechanism */

typedef void (*ast_handler_t)(uint32_t astprm);

/* Queue an AST for delivery */
uint32_t ast_queue(ast_handler_t handler, uint32_t param, uint8_t acmode);

/* Enable/disable AST delivery */
uint32_t ast_set_enable(int enable);

/* Deliver any pending ASTs (called from signal handler) */
void ast_deliver_pending(void);

/* Initialize AST subsystem (set up signal handlers) */
void ast_init(void);

/* Cleanup AST subsystem */
void ast_cleanup(void);

/* Check if ASTs are enabled */
int ast_is_enabled(void);

/* Get count of pending ASTs */
int ast_pending_count(void);

#endif
