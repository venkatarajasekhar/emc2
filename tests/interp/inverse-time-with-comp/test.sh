#!/bin/bash
rs274 -v test.var -t test.tbl -g inverse.ngc | awk '{$1=""; print}'
result=$?
[ -f test.var.bak ] && mv test.var.bak test.var
exit $result
