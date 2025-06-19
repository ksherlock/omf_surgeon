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

quoted characters are themselves.

```
file: segment* ; 

segment : SEGMENT label_or_star `{`
  statement* 
'}' ;

label : [A-Za-z_~][A-Za-z0-9_~]* | '"' [^"]+ '"' ;
label_list : label | label_list ',' label ;
label_or_star : label | '*' ;

statement : alias_statement | delete_statement | strong_statement | weak_statement ';' ;

alias_statement : ALIAS label_list ;
delete_statement : DELETE ;
strong_statement : STRONG label_list ;
weak_statement : WEAK label_list ;

```
* segment name comparison is case sensitive.  if there is not a match, the wildcard (`*`) segment, if any, will be used.

* `alias` will add a public `GLOBAL` entry at the start of the segment. (*c.f.* ORCA/M's `ENTRY` statement)

* `delete` will delete the segment (ie, it won't be copied to the new file).

* `strong` will convert all soft references to hard references.  If there are no references, a `STRONG` entry will be added to the end of the segment.

* `weak` will convert all hard references to soft references and delete any `STRONG` entries.

