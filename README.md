# omf_surgeon
OMF Surgeon

placeholder ... for now.

Add a new init segment to an existing OMF executable.  Sort of a linker for something that's already linked.

INIT segments are executed after the file loads and is relocated but before it starts executing.  This would allow for fun stuff like patching bugs, etc.
