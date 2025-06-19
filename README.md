OMF Surgeon
-----------

```
omf_surgeon [-hv] scriptfile infile outfile
```

copies `infile` to `outfile`, using `scriptfile` to make modifications.

`infile` and `outfile` are OMF object files.

Script File
-----------

UPPERCASE WORDS are (context-sensitive) keywords, which are actually lowercase.

lowercase words are non-terminal.

'quoted characters' are themselves.

```
file ::= segment* ; 

segment ::= SEGMENT label_or_star `{`
  statement* 
'}' ;

label ::= [A-Za-z_~][A-Za-z0-9_~]* | '"' [^"]+ '"' ;
label_list ::= label [ ',' label ]* ;
label_or_star ::= label | '*' ;
number ::= '$' [0-9A-F]+ | [0-9]+ | '%' [01][01_]* ;


statement ::= alias_stmt 
            | delete_stmt
            | strong_stmt
            | weak_stmt
            | kind_stmt
            | loadname_stmt
            ;

alias_stmt ::= ALIAS label_list ';' ;
delete_stmt ::= DELETE ';' ;
kind_stmt ::= KIND number ';' ;
loadname_stmt ::= LOADNAME label ';' ;
strong_stmt ::= STRONG label_list ';' ;
weak_stmt ::= WEAK label_list ';' ;

```
* '#' starts a comment (terminated by the end of the line).

* segment name comparison is case sensitive.  if there is no match, the wildcard (`*`) segment, if any, will be used.

* `alias` will add a public `GLOBAL` entry at the start of the segment. (*c.f.* ORCA/M's `ENTRY` statement)

* `delete` will delete the segment (ie, it won't be copied to the new file).

* `strong` will convert all soft references to hard references.  If there are no references, a `STRONG` entry will be added to the end of the segment.

* `weak` will convert all hard references to soft references and delete any `STRONG` entries.

* `kind` will set the segment header kind field.

* `loadname` will set the segment header loadname field (padded with ' ' to 10 characters).

