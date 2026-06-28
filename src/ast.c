#include "ast.h"

#include <stdio.h>
#include <stdlib.h>

void nodelist_init(NodeList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void nodelist_push(NodeList *list, Node *node) {
    if (list->count + 1 > list->capacity) {
        int cap = list->capacity < 8 ? 8 : list->capacity * 2;
        list->items = (Node **)realloc(list->items, sizeof(Node *) * cap);
        list->capacity = cap;
    }
    list->items[list->count++] = node;
}

/* ---------- pretty printer ---------- */

static void indent(int depth) {
    for (int i = 0; i < depth; i++) printf("  ");
}

static void print_node(Node *n, int depth);

static void print_list(NodeList *list, int depth) {
    for (int i = 0; i < list->count; i++) print_node(list->items[i], depth);
}

static void print_node(Node *n, int depth) {
    if (n == NULL) return;
    indent(depth);
    switch (n->type) {
        case NODE_PROGRAM:
            printf("Program\n");
            print_list(&n->as.program.declarations, depth + 1);
            break;
        case NODE_LITERAL:
            printf("Literal(%s) %s\n",
                   token_type_name(n->as.literal.kind), n->as.literal.text);
            break;
        case NODE_IDENT:
            printf("Ident %s\n", n->as.ident.name);
            break;
        case NODE_UNARY:
            printf("Unary '%s'\n", n->as.unary.op);
            print_node(n->as.unary.operand, depth + 1);
            break;
        case NODE_BINARY:
            printf("Binary '%s'\n", n->as.binary.op);
            print_node(n->as.binary.left, depth + 1);
            print_node(n->as.binary.right, depth + 1);
            break;
        case NODE_ASSIGN:
            printf("Assign '%s'\n", n->as.assign.op);
            print_node(n->as.assign.target, depth + 1);
            print_node(n->as.assign.value, depth + 1);
            break;
        case NODE_CALL:
            printf("Call\n");
            indent(depth + 1); printf("callee:\n");
            print_node(n->as.call.callee, depth + 2);
            if (n->as.call.args.count > 0) {
                indent(depth + 1); printf("args:\n");
                print_list(&n->as.call.args, depth + 2);
            }
            break;
        case NODE_GET:
            printf("Get %s'%s'\n", n->as.get.safe ? "?. " : ". ", n->as.get.name);
            print_node(n->as.get.object, depth + 1);
            break;
        case NODE_IMPORT:
            printf("Import %s%s%s\n", n->as.import.path,
                   n->as.import.alias ? " as " : "",
                   n->as.import.alias ? n->as.import.alias : "");
            break;
        case NODE_VAR_DECL:
            printf("VarDecl %s %s%s%s\n",
                   n->as.var_decl.is_mutable ? "var" : "let",
                   n->as.var_decl.name,
                   n->as.var_decl.decl_type ? " : " : "",
                   n->as.var_decl.decl_type ? n->as.var_decl.decl_type : "");
            if (n->as.var_decl.init) {
                indent(depth + 1); printf("init:\n");
                print_node(n->as.var_decl.init, depth + 2);
            }
            break;
        case NODE_FN_DECL:
            printf("FnDecl %s%s%s%s\n",
                   n->as.fn_decl.is_async ? "async " : "",
                   n->as.fn_decl.name,
                   n->as.fn_decl.ret_type ? " -> " : "",
                   n->as.fn_decl.ret_type ? n->as.fn_decl.ret_type : "");
            if (n->as.fn_decl.params.count > 0) {
                indent(depth + 1); printf("params:\n");
                print_list(&n->as.fn_decl.params, depth + 2);
            }
            indent(depth + 1); printf("body:\n");
            print_node(n->as.fn_decl.body, depth + 2);
            break;
        case NODE_PARAM:
            printf("Param %s : %s\n", n->as.param.name, n->as.param.param_type);
            break;
        case NODE_RETURN:
            printf("Return\n");
            print_node(n->as.ret.value, depth + 1);
            break;
        case NODE_IF:
            printf("If\n");
            indent(depth + 1); printf("cond:\n");
            print_node(n->as.if_stmt.cond, depth + 2);
            indent(depth + 1); printf("then:\n");
            print_node(n->as.if_stmt.then_branch, depth + 2);
            if (n->as.if_stmt.else_branch) {
                indent(depth + 1); printf("else:\n");
                print_node(n->as.if_stmt.else_branch, depth + 2);
            }
            break;
        case NODE_WHILE:
            printf("While\n");
            indent(depth + 1); printf("cond:\n");
            print_node(n->as.while_stmt.cond, depth + 2);
            indent(depth + 1); printf("body:\n");
            print_node(n->as.while_stmt.body, depth + 2);
            break;
        case NODE_FOR:
            printf("For %s in\n", n->as.for_stmt.var_name);
            print_node(n->as.for_stmt.iterable, depth + 1);
            indent(depth + 1); printf("body:\n");
            print_node(n->as.for_stmt.body, depth + 2);
            break;
        case NODE_STRUCT:
            printf("Struct %s\n", n->as.struct_decl.name);
            print_list(&n->as.struct_decl.fields, depth + 1);
            break;
        case NODE_RANGE:
            printf("Range\n");
            print_node(n->as.range.start, depth + 1);
            print_node(n->as.range.end, depth + 1);
            break;
        case NODE_MATCH:
            printf("Match\n");
            indent(depth + 1); printf("subject:\n");
            print_node(n->as.match.subject, depth + 2);
            print_list(&n->as.match.arms, depth + 1);
            break;
        case NODE_MATCH_ARM:
            if (n->as.match_arm.kind == 0) printf("Arm _ ->\n");
            else if (n->as.match_arm.kind == 2) printf("Arm bind %s ->\n", n->as.match_arm.bind);
            else { printf("Arm literal ->\n"); print_node(n->as.match_arm.literal, depth + 1); }
            print_node(n->as.match_arm.body, depth + 1);
            break;
        case NODE_BLOCK:
            printf("Block\n");
            print_list(&n->as.block.statements, depth + 1);
            break;
        case NODE_EXPR_STMT:
            printf("ExprStmt\n");
            print_node(n->as.expr_stmt.expr, depth + 1);
            break;
        case NODE_COMPONENT:
            printf("Component %s\n", n->as.component.name);
            if (n->as.component.members.count > 0) {
                indent(depth + 1); printf("members:\n");
                print_list(&n->as.component.members, depth + 2);
            }
            indent(depth + 1); printf("render:\n");
            print_node(n->as.component.render_body, depth + 2);
            break;
        case NODE_STATE_DECL:
            printf("StateDecl %s : %s\n",
                   n->as.state_decl.name, n->as.state_decl.decl_type);
            if (n->as.state_decl.init) {
                indent(depth + 1); printf("init:\n");
                print_node(n->as.state_decl.init, depth + 2);
            }
            break;
        case NODE_UI_NODE:
            printf("UI %s\n", n->as.ui_node.name);
            if (n->as.ui_node.args.count > 0) {
                indent(depth + 1); printf("args:\n");
                print_list(&n->as.ui_node.args, depth + 2);
            }
            if (n->as.ui_node.modifiers.count > 0) {
                indent(depth + 1); printf("modifiers:\n");
                print_list(&n->as.ui_node.modifiers, depth + 2);
            }
            if (n->as.ui_node.children.count > 0) {
                indent(depth + 1); printf("children:\n");
                print_list(&n->as.ui_node.children, depth + 2);
            }
            break;
        case NODE_UI_ARG:
            if (n->as.ui_arg.label) printf("Arg %s:\n", n->as.ui_arg.label);
            else printf("Arg:\n");
            print_node(n->as.ui_arg.value, depth + 1);
            break;
        case NODE_MODIFIER:
            printf("Modifier .%s\n", n->as.modifier.name);
            print_list(&n->as.modifier.args, depth + 1);
            break;
    }
}

void ast_print(Node *node) {
    print_node(node, 0);
}
