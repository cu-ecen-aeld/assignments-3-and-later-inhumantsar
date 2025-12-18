#!/bin/bash
writefile=$1
writestr=$2

if [[ "$writefile" == "" || "$writestr" == "" ]]; then
    echo "Usage: writer.sh <file> <write string>"
    exit 1
fi

parent="$(dirname $writefile)"
if [[ ! -d "$parent" ]]; then
    mkdir -p "$parent" || (echo "ERROR: could not create dir $parent" && exit 1)
fi

echo "$writestr" > $writefile || (echo "ERROR: could not write file $writefile" && exit 1)