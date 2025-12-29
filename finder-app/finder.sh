#!/bin/sh
filesdir=$1
searchstr=$2

if [[ "$filesdir" == "" || "$searchstr" == "" ]]; then
    echo "Usage: finder.sh <directory> <search string>"
    exit 1
fi

if [[ ! -d "$filesdir" ]]; then
    echo "ERROR: $filesdir does not exist or is not a directory!"
    exit 1
fi

files=0
lines=0

for f in $(find -L $filesdir -type f); do
    files=$(($files + 1))
    l=$(grep "$searchstr" $f | wc -l)
    lines=$(($lines + $l))
done

echo "The number of files are $files and the number of matching lines are $lines."