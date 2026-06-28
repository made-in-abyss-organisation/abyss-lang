# Abyss Language Specification (draft)

This is a living document. The grammar below defines the core of Abyss; it
grows as the language matures.

## Philosophy

Abyss is a statically-typed, AOT-compiled language for mobile development.
Memory is managed by **Automatic Reference Counting (ARC)** — no garbage
collector, hence no GC pauses, hence smooth UI. Reactive UI is part of the
language itself via `component` / `state` / `render`.

## Lexical structure

- **Comments**: `// to end of line`.
- **Newlines are significant** — they terminate statements (no semicolons).
- **Identifiers**: `[A-Za-z_][A-Za-z0-9_]*`.
- **Int**: `42`. **Float**: `3.14`. **String**: `"..."` with `${expr}`
  interpolation.
- **Keywords**: `let var fn struct component state render import as async
  await return if else match for in true false nil`.

## Memory model

- Every reference type is reference-counted. Retain/release are inserted by
  the compiler.
- Reference cycles are broken with `weak` references (planned).
- Value types (`Int`, `Float`, `Bool`, `struct` by default) are copied.

## Null safety

- A type is non-nullable unless suffixed with `?` (e.g. `String?`).
- `?.` performs a safe call; `??` provides a default.

## Grammar (EBNF)

```ebnf
(* ---------- Program structure ---------- *)
program        = { declaration } ;
declaration    = importDecl | fnDecl | structDecl | componentDecl | varDecl ;

importDecl     = "import" , STRING , [ "as" , IDENT ] ;

(* ---------- Declarations ---------- *)
varDecl        = ( "let" | "var" ) , IDENT , [ ":" , type ] , [ "=" , expr ] ;

fnDecl         = [ "async" ] , "fn" , IDENT , "(" , [ params ] , ")" ,
                 [ "->" , type ] , ( block | "=" , expr ) ;
params         = param , { "," , param } ;
param          = IDENT , ":" , type , [ "=" , expr ] ;

structDecl     = "struct" , IDENT , "{" , { field | fnDecl } , "}" ;
field          = ( "let" | "var" ) , IDENT , ":" , type ;

componentDecl  = "component" , IDENT , "{" ,
                   { stateDecl | fnDecl } ,
                   "render" , "{" , uiNode , "}" ,
                 "}" ;
stateDecl      = "state" , IDENT , ":" , type , [ "=" , expr ] ;

(* ---------- UI tree (the special sauce) ---------- *)
uiNode         = IDENT , [ "(" , [ uiArgs ] , ")" ] ,
                 [ "{" , { uiNode } , "}" ] ,
                 { modifier } ;
uiArgs         = uiArg , { "," , uiArg } ;
uiArg          = [ IDENT , ":" ] , expr ;          (* named or positional *)
modifier       = "." , IDENT , "(" , [ uiArgs ] , ")" ;

(* ---------- Types ---------- *)
type           = IDENT , [ "<" , type , { "," , type } , ">" ] , [ "?" ] ;

(* ---------- Statements ---------- *)
block          = "{" , { statement } , "}" ;
statement      = varDecl | ifStmt | matchStmt | forStmt
               | returnStmt | exprStmt ;
ifStmt         = "if" , expr , block , [ "else" , ( ifStmt | block ) ] ;
matchStmt      = "match" , expr , "{" , { matchArm } , "}" ;
matchArm       = pattern , "->" , ( expr | block ) ;
forStmt        = "for" , IDENT , "in" , expr , block ;
returnStmt     = "return" , [ expr ] ;
exprStmt       = expr ;

(* ---------- Expressions (precedence climbing) ---------- *)
expr           = assignment ;
assignment     = logicOr , [ ( "=" | "+=" | "-=" ) , assignment ] ;
logicOr        = logicAnd , { "||" , logicAnd } ;
logicAnd       = equality , { "&&" , equality } ;
equality       = comparison , { ( "==" | "!=" ) , comparison } ;
comparison     = term , { ( "<" | ">" | "<=" | ">=" ) , term } ;
term           = factor , { ( "+" | "-" ) , factor } ;
factor         = unary , { ( "*" | "/" | "%" ) , unary } ;
unary          = ( "!" | "-" ) , unary | postfix ;
postfix        = primary , { "." , IDENT | "?." , IDENT
                           | "(" , [ args ] , ")" | "??" , unary } ;
primary        = NUMBER | STRING | "true" | "false" | "nil"
               | IDENT | "(" , expr , ")" | interpString ;
interpString   = '"' , { CHAR | "${" , expr , "}" } , '"' ;
args           = expr , { "," , expr } ;

pattern        = IDENT [ "(" , [ IDENT { "," , IDENT } ] , ")" ] | "_" ;
```

The two identity-defining productions are `componentDecl` (UI built into the
language) and `uiNode` / `modifier` (the declarative widget tree with
chainable modifiers like `.size(24).bold()`).
