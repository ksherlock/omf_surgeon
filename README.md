OMF Surgeon
-----------

```
omf_surgeon [-hv] scriptfile infile outfile
```

copies `infile` to `outfile`, using `scriptfile` to make modifications.

`infile` and `outfile` are OMF object files.

Script File
-----------


```
file          ::= segment* ; 

segment       ::= 'segment' label_or_star `{` statement* '}' ;

statement     ::= alias_stmt 
                | delete_stmt
                | strong_stmt
                | weak_stmt
                | kind_stmt
                | loadname_stmt
                ;

alias_stmt    ::= 'alias' label_list ';' ;
delete_stmt   ::= 'delete' ';' ;
kind_stmt     ::= 'kind' number ';' ;
loadname_stmt ::= 'loadname' label ';' ;
strong_stmt   ::= 'strong' label_list ';' ;
weak_stmt     ::= 'weak' label_list ';' ;


label         ::= [A-Za-z_~][A-Za-z0-9_~]* | '"' [^"]+ '"' ;
label_list    ::= label [ ',' label ]* ;
label_or_star ::= label | '*' ;
number        ::= '$' [0-9A-F]+ | '%' [01][01_]* | [0-9]+  ;



```
* `#` starts a comment (terminated by the end of the line).

* segment name comparison is case sensitive.  if there is no match, the wildcard (`*`) segment, if any, will be used.

* `alias` will add a public `GLOBAL` entry at the start of the segment. (*c.f.* ORCA/M's `ENTRY` statement)

* `delete` will delete the segment (ie, it won't be copied to the new file).

* `strong` will convert all soft references to hard references.  If there are no references, a `STRONG` entry will be added to the end of the segment.

* `weak` will convert all hard references to soft references and delete any `STRONG` entries.

* `kind` will set the segment header kind field.

* `loadname` will set the segment header loadname field (padded with ' ' to 10 characters).

